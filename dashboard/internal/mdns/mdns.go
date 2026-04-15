// Package mdns implements a minimal mDNS/DNS-SD responder that
// advertises the Pi-Clock dashboard on the local network.
//
// It announces:
//   - A record for {hostname}.local  →  IP addresses
//   - PTR/SRV/TXT records for _http._tcp and _https._tcp services
//
// The responder continuously monitors network interfaces. When an
// interface gains an IPv4 address it is bound immediately and an
// announcement burst is sent (RFC 6762 §8.3).  When an interface
// loses its address or goes down, a goodbye (TTL=0) is sent and
// the listener is closed.  Periodic re-announcements keep caches
// fresh.
//
// Loopback interfaces are always skipped.
//
// Concurrency: a single monitor goroutine owns the interface map
// and performs all add/remove/announce operations.  Per-interface
// serve goroutines handle incoming queries.  Close() signals stop
// and waits for all goroutines to finish.
package mdns

import (
	"log/slog"
	"net"
	"os"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/miekg/dns"
)

const (
	mdnsPort   = 5353
	defaultTTL = 120

	// maxQuestions caps the number of questions processed per query
	// to prevent resource exhaustion from oversized packets.
	maxQuestions = 16

	// maxAnswers caps the total answer records in a single response
	// to limit amplification and memory use.
	maxAnswers = 32

	// queryRateLimit is the maximum queries processed per second
	// per interface before dropping.
	queryRateLimit = 50

	// monitorInterval is how often the monitor goroutine checks for
	// interface changes (new interfaces, removed interfaces, IP changes).
	monitorInterval = 10 * time.Second

	// monitorFastInterval is used when no interfaces are bound, so that
	// late-arriving interfaces (e.g. eth0 getting DHCP after MCP starts)
	// are picked up quickly.
	monitorFastInterval = 2 * time.Second

	// announceEvery controls how often periodic re-announcements are
	// sent, as a multiple of monitorInterval (6 × 10s = 60s).
	announceEvery = 6
)

// validProto matches DNS-SD protocol labels per RFC 6335: starts with
// a letter, contains alphanumeric or hyphens, must not end with hyphen.
var validProto = regexp.MustCompile(`^[a-zA-Z]([a-zA-Z0-9-]{0,13}[a-zA-Z0-9])?$`)

// hostnameChars matches valid mDNS hostname characters.
var hostnameChars = regexp.MustCompile(`[^a-zA-Z0-9-]`)

// ServicePort describes one TCP service to advertise via DNS-SD.
type ServicePort struct {
	Proto string   // "http", "https", or custom (e.g. "pistar")
	Port  int      // TCP port number
	Txt   []string // optional TXT key=value pairs (default: "path=/")
}

// Responder announces the local hostname and services via mDNS and
// answers queries on every active network interface.
type Responder struct {
	hostname     string        // FQDN, e.g. "pi-clock.local."
	instanceBase string        // short hostname for instance names
	services     []ServicePort // services to advertise (read-only after Start)

	// ifaces is owned exclusively by the monitor goroutine — no
	// mutex needed for map access since only one goroutine touches it.
	ifaces map[string]*ifaceEntry

	stop chan struct{}
	wg   sync.WaitGroup
}

// ifaceEntry tracks a bound interface: the multicast connection, the
// IP addresses to advertise, and a channel that is closed when the
// serve goroutine exits.
type ifaceEntry struct {
	name   string
	conn   *net.UDPConn
	ips    []net.IP   // IPv4 addresses
	ip6s   []net.IP   // IPv6 addresses (global/ULA only, not link-local)
	exited chan struct{} // closed when serve goroutine returns
}

// ifaceInfo captures what scanInterfaces found for a single interface.
type ifaceInfo struct {
	iface net.Interface
	ips   []net.IP // IPv4
	ip6s  []net.IP // IPv6 (global/ULA only)
}

// rateLimiter tracks query rate using a token bucket with CAS-based
// reset to avoid races.  Each serve goroutine owns its own instance.
type rateLimiter struct {
	tokens    atomic.Int64
	lastReset atomic.Int64 // unix seconds
}

func (rl *rateLimiter) allow() bool {
	now := time.Now().Unix()
	for {
		last := rl.lastReset.Load()
		if now > last {
			if rl.lastReset.CompareAndSwap(last, now) {
				rl.tokens.Store(queryRateLimit)
				break
			}
			// Another goroutine beat us — re-check.
			continue
		}
		break
	}
	for {
		t := rl.tokens.Load()
		if t <= 0 {
			return false
		}
		if rl.tokens.CompareAndSwap(t, t-1) {
			return true
		}
	}
}

// ----- Public API -----------------------------------------------------------

// Start creates a Responder and begins monitoring network interfaces.
// Interfaces that are up with IPv4 addresses are bound immediately;
// others are picked up as they come online.  It is best-effort —
// errors are logged but do not prevent the application from running.
func Start(services []ServicePort) *Responder {
	shortHost := sanitizeHostname(resolveHostname())
	hostname := shortHost + ".local."

	// Filter out services with invalid protocol names.
	var valid []ServicePort
	for _, svc := range services {
		if !validProto.MatchString(svc.Proto) {
			slog.Warn("mdns: skipping service with invalid proto", "proto", svc.Proto)
			continue
		}
		valid = append(valid, svc)
	}

	r := &Responder{
		hostname:     hostname,
		instanceBase: shortHost,
		services:     valid,
		ifaces:       make(map[string]*ifaceEntry),
		stop:         make(chan struct{}),
	}

	r.wg.Add(1)
	go r.monitor()

	return r
}

// Close stops the responder, sends goodbye packets on all interfaces,
// and releases all sockets.
func (r *Responder) Close() {
	close(r.stop)
	r.wg.Wait()
	slog.Info("mDNS responder stopped")
}

// ----- Interface monitoring -------------------------------------------------

// monitor polls for interface changes and handles announcements.
// It is the sole owner of r.ifaces.
func (r *Responder) monitor() {
	defer r.wg.Done()

	// Immediate first scan.
	r.reconcile(true)

	// Use fast polling until at least one interface is bound, so that
	// late-arriving interfaces (eth0 via DHCP, USB WiFi hotplug) are
	// picked up within a couple of seconds rather than 10.
	interval := monitorFastInterval
	if len(r.ifaces) > 0 {
		interval = monitorInterval
	}
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	tick := 0

	for {
		select {
		case <-r.stop:
			r.teardownAll()
			return
		case <-ticker.C:
			tick++
			r.reconcile(tick%announceEvery == 0)

			// Switch between fast and normal polling based on
			// whether we have any bound interfaces.
			newInterval := monitorFastInterval
			if len(r.ifaces) > 0 {
				newInterval = monitorInterval
			}
			if newInterval != interval {
				interval = newInterval
				ticker.Reset(interval)
				if interval == monitorInterval {
					tick = 0 // reset announce counter on transition
				}
			}
		}
	}
}

// scanInterfaces returns all up, non-loopback, multicast-capable
// interfaces that have at least one IPv4 address.
func scanInterfaces() map[string]ifaceInfo {
	result := make(map[string]ifaceInfo)
	ifaces, err := net.Interfaces()
	if err != nil {
		slog.Warn("mdns: cannot list interfaces", "error", err)
		return result
	}

	for _, iface := range ifaces {
		if iface.Flags&net.FlagUp == 0 ||
			iface.Flags&net.FlagLoopback != 0 ||
			iface.Flags&net.FlagMulticast == 0 {
			continue
		}

		addrs, err := iface.Addrs()
		if err != nil {
			continue
		}
		var ips []net.IP
		var ip6s []net.IP
		for _, addr := range addrs {
			if ipnet, ok := addr.(*net.IPNet); ok {
				if ip4 := ipnet.IP.To4(); ip4 != nil {
					ips = append(ips, ip4)
				} else if ip6 := ipnet.IP.To16(); ip6 != nil && !ip6.IsLinkLocalUnicast() {
					ip6s = append(ip6s, ip6)
				}
			}
		}
		if len(ips) > 0 {
			result[iface.Name] = ifaceInfo{iface: iface, ips: ips, ip6s: ip6s}
		}
	}

	return result
}

// reconcile compares the desired interface state (from the OS) with
// the current state (r.ifaces) and adds/removes as needed.
func (r *Responder) reconcile(reannounce bool) {
	wanted := scanInterfaces()

	// Phase 1: collect interfaces to remove (gone or IPs changed).
	var toRemove []*ifaceEntry
	for name, entry := range r.ifaces {
		info, exists := wanted[name]
		if !exists || !sameIPs(entry.ips, info.ips) || !sameIPs(entry.ip6s, info.ip6s) {
			toRemove = append(toRemove, entry)
			delete(r.ifaces, name)
		}
	}

	// Phase 2: collect interfaces to add.
	type addItem struct {
		name string
		info ifaceInfo
	}
	var toAdd []addItem
	for name, info := range wanted {
		if _, exists := r.ifaces[name]; !exists {
			toAdd = append(toAdd, addItem{name, info})
		}
	}

	// Phase 3: teardown removed interfaces.
	for _, e := range toRemove {
		r.sendGoodbye(e)
		r.teardownIface(e)
	}

	// Phase 4: bind and announce new interfaces.
	for _, a := range toAdd {
		if r.stopped() {
			return
		}
		r.addIface(a.name, a.info)
	}

	// Phase 5: periodic re-announcement on all current interfaces.
	if reannounce {
		dst := &net.UDPAddr{IP: net.ParseIP("224.0.0.251"), Port: mdnsPort}
		for _, e := range r.ifaces {
			r.sendRecords(e, dst, defaultTTL)
		}
	}
}

// addIface binds a multicast listener on the given interface, starts
// a serve goroutine, and sends an announcement burst.
func (r *Responder) addIface(name string, info ifaceInfo) {
	group := net.UDPAddr{IP: net.ParseIP("224.0.0.251"), Port: mdnsPort}
	iface := info.iface
	conn, err := net.ListenMulticastUDP("udp4", &iface, &group)
	if err != nil {
		slog.Warn("mdns: listen failed", "iface", name, "error", err)
		return
	}

	e := &ifaceEntry{
		name:   name,
		conn:   conn,
		ips:    info.ips,
		ip6s:   info.ip6s,
		exited: make(chan struct{}),
	}
	r.ifaces[name] = e

	r.wg.Add(1)
	go r.serve(e)

	slog.Info("mDNS started on interface",
		"hostname", strings.TrimSuffix(r.hostname, "."),
		"iface", name,
		"ips", info.ips)

	// Announcement burst: 3 unsolicited responses at 1s intervals
	// per RFC 6762 §8.3.
	dst := &net.UDPAddr{IP: net.ParseIP("224.0.0.251"), Port: mdnsPort}
	for i := 0; i < 3; i++ {
		r.sendRecords(e, dst, defaultTTL)
		t := time.NewTimer(time.Second)
		select {
		case <-r.stop:
			t.Stop()
			return
		case <-t.C:
		}
	}
}

// teardownIface closes the connection and waits for the serve
// goroutine to exit.
func (r *Responder) teardownIface(e *ifaceEntry) {
	e.conn.Close()
	<-e.exited
	slog.Info("mDNS stopped on interface", "iface", e.name)
}

// teardownAll sends goodbyes and tears down every active interface.
// Called by monitor on shutdown.
func (r *Responder) teardownAll() {
	dst := &net.UDPAddr{IP: net.ParseIP("224.0.0.251"), Port: mdnsPort}
	for _, e := range r.ifaces {
		r.sendRecords(e, dst, 0) // goodbye
	}
	for name, e := range r.ifaces {
		e.conn.Close()
		<-e.exited
		delete(r.ifaces, name)
	}
}

// stopped returns true if the stop channel has been closed.
func (r *Responder) stopped() bool {
	select {
	case <-r.stop:
		return true
	default:
		return false
	}
}

// sameIPs returns true if the two IP slices contain the same addresses
// (order-independent).
func sameIPs(a, b []net.IP) bool {
	if len(a) != len(b) {
		return false
	}
	set := make(map[string]struct{}, len(a))
	for _, ip := range a {
		set[ip.String()] = struct{}{}
	}
	for _, ip := range b {
		if _, ok := set[ip.String()]; !ok {
			return false
		}
	}
	return true
}

// ----- DNS-SD record builders -----------------------------------------------

// serviceType returns e.g. "_http._tcp.local."
func serviceType(proto string) string {
	return "_" + proto + "._tcp.local."
}

// instanceName returns e.g. "pi-clock._http._tcp.local." using the
// actual hostname rather than a hardcoded value.
func (r *Responder) instanceName(proto string) string {
	return r.instanceBase + "." + serviceType(proto)
}

// buildAllRecords returns the full set of A + AAAA + PTR + SRV + TXT
// records for announcements.  If ttl is 0, peers treat it as a goodbye.
func (r *Responder) buildAllRecords(ips, ip6s []net.IP, ttl uint32) []dns.RR {
	var rrs []dns.RR

	// A records for the hostname
	for _, ip := range ips {
		rrs = append(rrs, &dns.A{
			Hdr: dns.RR_Header{
				Name:   r.hostname,
				Rrtype: dns.TypeA,
				Class:  dns.ClassINET | 0x8000,
				Ttl:    ttl,
			},
			A: ip,
		})
	}

	// AAAA records for the hostname (global/ULA IPv6 only)
	for _, ip := range ip6s {
		rrs = append(rrs, &dns.AAAA{
			Hdr: dns.RR_Header{
				Name:   r.hostname,
				Rrtype: dns.TypeAAAA,
				Class:  dns.ClassINET | 0x8000,
				Ttl:    ttl,
			},
			AAAA: ip,
		})
	}

	// NSEC record asserting which address types exist for this hostname
	// (RFC 6762 §6).  Prevents clients waiting for responses that will
	// never arrive (e.g. AAAA when only IPv4 is available).
	nsecTypes := []uint16{dns.TypeA}
	if len(ip6s) > 0 {
		nsecTypes = append(nsecTypes, dns.TypeAAAA)
	}
	rrs = append(rrs, &dns.NSEC{
		Hdr: dns.RR_Header{
			Name:   r.hostname,
			Rrtype: dns.TypeNSEC,
			Class:  dns.ClassINET | 0x8000,
			Ttl:    ttl,
		},
		NextDomain: r.hostname,
		TypeBitMap: nsecTypes,
	})

	// Service records for each port
	for _, svc := range r.services {
		if svc.Port == 0 {
			continue
		}
		st := serviceType(svc.Proto)
		inst := r.instanceName(svc.Proto)

		// PTR: _http._tcp.local. → pi-clock._http._tcp.local.
		rrs = append(rrs, &dns.PTR{
			Hdr: dns.RR_Header{
				Name:   st,
				Rrtype: dns.TypePTR,
				Class:  dns.ClassINET,
				Ttl:    ttl,
			},
			Ptr: inst,
		})

		// SRV: pi-clock._http._tcp.local. → pi-clock.local.:port
		rrs = append(rrs, &dns.SRV{
			Hdr: dns.RR_Header{
				Name:   inst,
				Rrtype: dns.TypeSRV,
				Class:  dns.ClassINET | 0x8000,
				Ttl:    ttl,
			},
			Port:   uint16(svc.Port),
			Target: r.hostname,
		})

		// TXT: pi-clock._http._tcp.local.
		txt := svc.Txt
		if len(txt) == 0 {
			txt = []string{"path=/"}
		}
		rrs = append(rrs, &dns.TXT{
			Hdr: dns.RR_Header{
				Name:   inst,
				Rrtype: dns.TypeTXT,
				Class:  dns.ClassINET | 0x8000,
				Ttl:    ttl,
			},
			Txt: txt,
		})
	}

	return rrs
}

// ----- Announcements --------------------------------------------------------

// sendRecords sends a single mDNS response with the full record set
// for the given interface.
func (r *Responder) sendRecords(e *ifaceEntry, dst *net.UDPAddr, ttl uint32) {
	rrs := r.buildAllRecords(e.ips, e.ip6s, ttl)
	resp := dns.Msg{
		MsgHdr: dns.MsgHdr{Response: true, Authoritative: true},
		Answer: rrs,
	}
	if out, err := resp.Pack(); err == nil {
		if _, err := e.conn.WriteToUDP(out, dst); err != nil {
			slog.Debug("mdns: write failed", "iface", e.name, "error", err)
		}
	}
}

// sendGoodbye sends a goodbye announcement (TTL=0) for the given
// interface, telling peers to flush cached records.
func (r *Responder) sendGoodbye(e *ifaceEntry) {
	dst := &net.UDPAddr{IP: net.ParseIP("224.0.0.251"), Port: mdnsPort}
	r.sendRecords(e, dst, 0)
}

// ----- Query handling -------------------------------------------------------

// serve reads mDNS queries on a single interface and responds to
// matching questions.  It exits when the connection is closed.
func (r *Responder) serve(e *ifaceEntry) {
	defer r.wg.Done()
	defer close(e.exited)

	buf := make([]byte, 1500)
	rl := &rateLimiter{}
	rl.tokens.Store(queryRateLimit)
	rl.lastReset.Store(time.Now().Unix())

	for {
		n, src, err := e.conn.ReadFromUDP(buf)
		if err != nil {
			// On global stop or conn close, exit cleanly.
			select {
			case <-r.stop:
				return
			default:
			}
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				continue
			}
			return // conn closed by teardownIface
		}

		if !rl.allow() {
			continue // rate limited
		}

		// Copy packet before parsing — miekg/dns may alias
		// into the buffer for name decompression.
		pkt := make([]byte, n)
		copy(pkt, buf[:n])

		var msg dns.Msg
		if err := msg.Unpack(pkt); err != nil {
			continue // ignore malformed packets
		}

		if msg.MsgHdr.Response {
			continue
		}

		r.handleQuery(e, &msg, src)
	}
}

// handleQuery checks each question and builds a combined response.
// If any question has the QU bit set (RFC 6762 §5.4), the response
// is sent unicast to the querier; otherwise it is multicast.
func (r *Responder) handleQuery(e *ifaceEntry, query *dns.Msg, src *net.UDPAddr) {
	var answers []dns.RR
	unicast := false
	needNSEC := false

	questions := query.Question
	if len(questions) > maxQuestions {
		questions = questions[:maxQuestions]
	}

	for _, q := range questions {
		if q.Qclass&0x8000 != 0 {
			unicast = true
		}
		qname := strings.ToLower(q.Name)

		// Address records for our hostname
		if qname == strings.ToLower(r.hostname) {
			if q.Qtype == dns.TypeA || q.Qtype == dns.TypeANY {
				for _, ip := range e.ips {
					answers = append(answers, &dns.A{
						Hdr: dns.RR_Header{
							Name:   r.hostname,
							Rrtype: dns.TypeA,
							Class:  dns.ClassINET | 0x8000,
							Ttl:    defaultTTL,
						},
						A: ip,
					})
					if len(answers) >= maxAnswers {
						break
					}
				}
			}
			if q.Qtype == dns.TypeAAAA || q.Qtype == dns.TypeANY {
				for _, ip := range e.ip6s {
					answers = append(answers, &dns.AAAA{
						Hdr: dns.RR_Header{
							Name:   r.hostname,
							Rrtype: dns.TypeAAAA,
							Class:  dns.ClassINET | 0x8000,
							Ttl:    defaultTTL,
						},
						AAAA: ip,
					})
					if len(answers) >= maxAnswers {
						break
					}
				}
			}
			needNSEC = true
		}
		if len(answers) >= maxAnswers {
			break
		}

		// Service discovery records
		for _, svc := range r.services {
			if svc.Port == 0 {
				continue
			}
			st := strings.ToLower(serviceType(svc.Proto))
			inst := strings.ToLower(r.instanceName(svc.Proto))

			// PTR query for service type
			if qname == st && (q.Qtype == dns.TypePTR || q.Qtype == dns.TypeANY) {
				answers = append(answers, &dns.PTR{
					Hdr: dns.RR_Header{
						Name:   serviceType(svc.Proto),
						Rrtype: dns.TypePTR,
						Class:  dns.ClassINET,
						Ttl:    defaultTTL,
					},
					Ptr: r.instanceName(svc.Proto),
				})
			}

			// SRV/TXT query for instance name
			if qname == inst {
				if q.Qtype == dns.TypeSRV || q.Qtype == dns.TypeANY {
					answers = append(answers, &dns.SRV{
						Hdr: dns.RR_Header{
							Name:   r.instanceName(svc.Proto),
							Rrtype: dns.TypeSRV,
							Class:  dns.ClassINET | 0x8000,
							Ttl:    defaultTTL,
						},
						Port:   uint16(svc.Port),
						Target: r.hostname,
					})
				}
				if q.Qtype == dns.TypeTXT || q.Qtype == dns.TypeANY {
					txt := svc.Txt
					if len(txt) == 0 {
						txt = []string{"path=/"}
					}
					answers = append(answers, &dns.TXT{
						Hdr: dns.RR_Header{
							Name:   r.instanceName(svc.Proto),
							Rrtype: dns.TypeTXT,
							Class:  dns.ClassINET | 0x8000,
							Ttl:    defaultTTL,
						},
						Txt: txt,
					})
				}
			}

			if len(answers) >= maxAnswers {
				break
			}
		}
		if len(answers) >= maxAnswers {
			break
		}
	}

	if len(answers) == 0 {
		return
	}

	// RFC 6762 §6: include NSEC in Additional to assert which address
	// types exist, so clients don't wait for responses that never arrive.
	var extra []dns.RR
	if needNSEC {
		nsecTypes := []uint16{dns.TypeA}
		if len(e.ip6s) > 0 {
			nsecTypes = append(nsecTypes, dns.TypeAAAA)
		}
		extra = append(extra, &dns.NSEC{
			Hdr: dns.RR_Header{
				Name:   r.hostname,
				Rrtype: dns.TypeNSEC,
				Class:  dns.ClassINET | 0x8000,
				Ttl:    defaultTTL,
			},
			NextDomain: r.hostname,
			TypeBitMap: nsecTypes,
		})
	}

	truncated := len(answers) >= maxAnswers
	resp := dns.Msg{
		MsgHdr: dns.MsgHdr{
			Response:      true,
			Authoritative: true,
			Truncated:     truncated,
		},
		Answer: answers,
		Extra:  extra,
	}
	out, err := resp.Pack()
	if err != nil {
		return
	}

	dst := &net.UDPAddr{IP: net.ParseIP("224.0.0.251"), Port: mdnsPort}
	if unicast && src != nil {
		dst = src
	}
	if _, err := e.conn.WriteToUDP(out, dst); err != nil {
		slog.Debug("mdns: response write failed", "iface", e.name, "error", err)
	}
}

// ----- Helpers --------------------------------------------------------------

// resolveHostname returns the short system hostname (without domain).
// Falls back to "pi-clock" if the hostname cannot be determined.
func resolveHostname() string {
	h, err := os.Hostname()
	if err != nil {
		return "pi-clock"
	}
	if idx := strings.IndexByte(h, '.'); idx != -1 {
		h = h[:idx]
	}
	if h == "" {
		return "pi-clock"
	}
	return h
}

// sanitizeHostname restricts a hostname to valid mDNS characters
// (letters, digits, hyphens) and enforces the 63-character DNS
// label limit.
func sanitizeHostname(h string) string {
	h = hostnameChars.ReplaceAllString(h, "")
	h = strings.Trim(h, "-")
	if len(h) > 63 {
		h = h[:63]
	}
	if h == "" {
		return "pi-clock"
	}
	return h
}

// PortFromAddr extracts the port number from a listen address like
// ":443" or "0.0.0.0:8443". Returns 0 if parsing fails or the port
// is out of the valid TCP range.
func PortFromAddr(addr string) int {
	_, portStr, err := net.SplitHostPort(addr)
	if err != nil {
		portStr = strings.TrimLeft(addr, ":")
	}
	port, err := strconv.Atoi(portStr)
	if err != nil {
		slog.Warn("mdns: invalid port in address", "addr", addr)
		return 0
	}
	if port < 1 || port > 65535 {
		slog.Warn("mdns: port out of range", "addr", addr, "port", port)
		return 0
	}
	return port
}

// FormatService is a convenience to build a ServicePort from a
// protocol name and listen address string.
func FormatService(proto, listenAddr string) ServicePort {
	return ServicePort{Proto: proto, Port: PortFromAddr(listenAddr)}
}
