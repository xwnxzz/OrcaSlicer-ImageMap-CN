// DSH-PATCH: self-implemented Bambu LAN print sender (see header for rationale).
//
// 1. FTPS  implicit TLS on 990 via libcurl  -> upload <name>.gcode.3mf + sidecar .bbl to /cache
// 2. MQTT  TSL on 8883, minimal 3.1.1 client over OpenSSL -> publish project_file
//
// Compiled into the application and also usable stand-alone for testing.

#include "BambuLanSender.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include <curl/curl.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/md5.h>

#include <boost/log/trivial.hpp>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET dsh_socket_t;
  #define DSH_INVALID_SOCKET INVALID_SOCKET
  #define dsh_closesocket   closesocket
#else
  #include <sys/socket.h>
  #include <netdb.h>
  #include <unistd.h>
  typedef int dsh_socket_t;
  #define DSH_INVALID_SOCKET (-1)
  #define dsh_closesocket   close
#endif

namespace Slic3r {

// ---------------------------------------------------------------- small helpers

static std::string md5_upper_hex_of_file(const std::string &path, uint64_t &size_out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    MD5_CTX ctx;
    MD5_Init(&ctx);
    std::vector<char> buf(1 << 20);
    uint64_t total = 0;
    while (f) {
        f.read(buf.data(), buf.size());
        std::streamsize n = f.gcount();
        if (n <= 0)
            break;
        MD5_Update(&ctx, buf.data(), (size_t) n);
        total += (uint64_t) n;
    }
    size_out = total;
    unsigned char d[16];
    MD5_Final(d, &ctx);
    char hex[33];
    for (int i = 0; i < 16; ++i)
        std::snprintf(hex + i * 2, 3, "%02X", (unsigned) d[i]);
    return std::string(hex, 32);
}

static std::string json_escape(const std::string &s)
{
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        default:
            if (c < 0x20) {
                char b[8];
                std::snprintf(b, sizeof(b), "\\u%04x", (unsigned) c);
                o += b;
            } else {
                o += (char) c;
            }
        }
    }
    return o;
}

// ---------------------------------------------------------------- FTPS via libcurl

static size_t dsh_curl_discard(char *, size_t size, size_t nmemb, void *)
{
    return size * nmemb;
}

static size_t dsh_curl_read_file(char *buffer, size_t size, size_t nitems, void *userp)
{
    std::ifstream *f = static_cast<std::ifstream *>(userp);
    const size_t want = size * nitems;
    if (!f || !f->good())
        return 0;
    f->read(buffer, (std::streamsize) want);
    return (size_t) f->gcount();
}

// in-memory upload source
struct DshMemBuf
{
    const std::string *data;
    size_t             pos;
};

static size_t dsh_curl_read_mem(char *buffer, size_t size, size_t nitems, void *userp)
{
    DshMemBuf *m = static_cast<DshMemBuf *>(userp);
    const size_t want = size * nitems;
    if (!m || !m->data)
        return 0;
    const size_t left = m->data->size() - m->pos;
    const size_t take = want < left ? want : left;
    if (take)
        std::memcpy(buffer, m->data->data() + m->pos, take);
    m->pos += take;
    return take;
}

// monotonic sequence_id: the firmware rejects reused/zero sequence ids
static std::string now_seq()
{
    static unsigned long counter = 0;
    ++counter;
#ifdef _WIN32
    unsigned long ms = (unsigned long) ::GetTickCount64();
#else
    unsigned long ms = (unsigned long) time(nullptr);
#endif
    return std::to_string(ms * 1000UL + (counter % 1000));
}

// Progress plumbing: a transfer owns a [lo,hi] slice of the overall 0..100 range.
struct DshProgress
{
    const BambuLanProgressFn *fn { nullptr };
    std::string               phase;
    int                       lo { 0 };
    int                       hi { 100 };
    int                       last_reported { -1 };
    bool                      aborted { false };

    // Maps a 0..100 figure inside this transfer onto the overall range.
    bool report(int inner)
    {
        if (!fn || !(*fn))
            return true;
        if (inner < 0)
            inner = 0;
        if (inner > 100)
            inner = 100;
        const int overall = lo + (hi - lo) * inner / 100;
        if (overall == last_reported)
            return true;
        // only advance; never let the bar move backwards
        if (overall < last_reported)
            return true;
        last_reported = overall;
        if (!(*fn)(overall, phase, std::string()))
            aborted = true;
        return !aborted;
    }
};

// Coarse rate limit so the UI is not flooded during a large upload.
struct DshXferCtx
{
    DshProgress *prog { nullptr };
    ULONG        last_ms { 0 };
};

static int dsh_curl_xferinfo(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t ultotal, curl_off_t ulnow)
{
    if (!clientp)
        return 0;
    DshXferCtx *cx = static_cast<DshXferCtx *>(clientp);
    if (!cx->prog)
        return 0;
    if (cx->prog->aborted)
        return 1;   // non-zero aborts the transfer
#ifdef _WIN32
    const ULONG now = (ULONG) ::GetTickCount64();
#else
    const ULONG now = (ULONG) time(nullptr) * 1000UL;
#endif
    if (now - cx->last_ms < 150)
        return 0;
    cx->last_ms = now;
    int pct = 0;
    if (ultotal > 0)
        pct = (int) (ulnow * 100 / ultotal);
    else if (dltotal > 0)
        pct = (int) (dlnow * 100 / dltotal);
    cx->prog->report(pct);
    if (cx->prog->aborted)
        return 1;
    return 0;
}

struct FtpTarget
{
    std::string ip;
    std::string code;
    long        port { 990 };
};

// Creates a remote directory once (best effort: "already exists" is fine).
// Needed because the printer's storage directories disappear after a card format.
static void dsh_ftp_mkdir_once(const FtpTarget &t, const std::string &dir)
{
    CURL *curl = ::curl_easy_init();
    if (!curl)
        return;
    const std::string url = "ftps://" + t.ip + ":" + std::to_string(t.port) + "/";
    char errbuf[CURL_ERROR_SIZE];
    errbuf[0] = '\0';
    struct curl_slist *q = nullptr;
    q = ::curl_slist_append(q, ("MKD " + dir).c_str());
    ::curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    ::curl_easy_setopt(curl, CURLOPT_USERPWD, ("bblp:" + t.code).c_str());
    ::curl_easy_setopt(curl, CURLOPT_USE_SSL, (long) CURLUSESSL_ALL);
    ::curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    ::curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    ::curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    ::curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    ::curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    ::curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    ::curl_easy_setopt(curl, CURLOPT_QUOTE, q);
    ::curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    CURLcode rc = ::curl_easy_perform(curl);
    BOOST_LOG_TRIVIAL(info) << "bambu_lan_sender: MKD " << dir << " -> "
                            << (rc == CURLE_OK ? "ok" : ::curl_easy_strerror(rc));
    ::curl_slist_free_all(q);
    ::curl_easy_cleanup(curl);
}

// Uploads one file (or in-memory buffer) into the printer's /cache directory.
// Uses implicit FTPS: ftps://host:990/cache/<name> with CURLOPT_USE_SSL=CURLUSESSL_ALL.
// The remote directory is part of the URL path so libcurl issues CWD /cache first; this
// matters because the print command references /cache/<name> and the printer resolves the
// job from there.
static bool dsh_ftp_upload(const FtpTarget &t, const std::string &remote_name,
                           const std::string &local_path, const std::string *memory,
                           long &http_code_out, std::string &err,
                           DshProgress *progress = nullptr)
{
    CURL *curl = ::curl_easy_init();
    if (!curl) {
        err = "curl_easy_init failed";
        return false;
    }
    std::ifstream file;

    const std::string url = "ftps://" + t.ip + ":" + std::to_string(t.port) + "/cache/" + remote_name;
    char errbuf[CURL_ERROR_SIZE];
    errbuf[0] = '\0';

    ::curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    ::curl_easy_setopt(curl, CURLOPT_USERPWD, ("bblp:" + t.code).c_str());
    ::curl_easy_setopt(curl, CURLOPT_USE_SSL, (long) CURLUSESSL_ALL);
    ::curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    ::curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    ::curl_easy_setopt(curl, CURLOPT_FTP_CREATE_MISSING_DIRS, (long) CURLFTP_CREATE_DIR_RETRY);
    ::curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    ::curl_easy_setopt(curl, CURLOPT_TIMEOUT, 900L);
    ::curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    ::curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    ::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, dsh_curl_discard);

    // live progress (and user cancellation) during the transfer
    DshXferCtx xfer_ctx;
    xfer_ctx.prog = progress;
    ::curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    ::curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, dsh_curl_xferinfo);
    ::curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &xfer_ctx);

    if (memory) {
        DshMemBuf mb { memory, 0 };
        ::curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        ::curl_easy_setopt(curl, CURLOPT_READFUNCTION, dsh_curl_read_mem);
        ::curl_easy_setopt(curl, CURLOPT_READDATA, &mb);
        ::curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t) memory->size());
        CURLcode rc = ::curl_easy_perform(curl);
        long code = 0;
        ::curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        http_code_out = code;
        if (rc != CURLE_OK) {
            err = errbuf[0] ? errbuf : ::curl_easy_strerror(rc);
            ::curl_easy_cleanup(curl);
            return false;
        }
        ::curl_easy_cleanup(curl);
        return true;
    }

    file.open(local_path, std::ios::binary);
    if (!file) {
        ::curl_easy_cleanup(curl);
        err = "cannot open local file: " + local_path;
        return false;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff fsize = file.tellg();
    file.seekg(0, std::ios::beg);

    ::curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    ::curl_easy_setopt(curl, CURLOPT_READFUNCTION, dsh_curl_read_file);
    ::curl_easy_setopt(curl, CURLOPT_READDATA, &file);
    ::curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t) fsize);

    CURLcode rc = ::curl_easy_perform(curl);
    long code = 0;
    ::curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    http_code_out = code;
    if (rc != CURLE_OK) {
        err = errbuf[0] ? errbuf : ::curl_easy_strerror(rc);
        ::curl_easy_cleanup(curl);
        return false;
    }
    ::curl_easy_cleanup(curl);
    return true;
}

// ---------------------------------------------------------------- minimal MQTT 3.1.1 client

class MqttTlsClient
{
public:
    ~MqttTlsClient() { close(); }

    bool connect(const std::string &host, int port, const std::string &user,
                 const std::string &pass, const std::string &client_id, std::string &err)
    {
#ifdef _WIN32
        static bool wsa_done = false;
        if (!wsa_done) {
            WSADATA w;
            if (WSAStartup(MAKEWORD(2, 2), &w) != 0) {
                err = "WSAStartup failed";
                return false;
            }
            wsa_done = true;
        }
#endif
        struct addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = nullptr;
        const std::string port_s = std::to_string(port);
        if (getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res) != 0 || !res) {
            err = "resolve failed: " + host;
            return false;
        }
        dsh_socket_t s = DSH_INVALID_SOCKET;
        for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
            s = (dsh_socket_t) ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (s == DSH_INVALID_SOCKET)
                continue;
            if (::connect(s, ai->ai_addr, (int) ai->ai_addrlen) == 0)
                break;
            dsh_closesocket(s);
            s = DSH_INVALID_SOCKET;
        }
        freeaddrinfo(res);
        if (s == DSH_INVALID_SOCKET) {
            err = "tcp connect failed to " + host + ":" + port_s;
            return false;
        }
        m_sock = s;

        // receive/send timeout so SSL_read cannot block forever
#ifdef _WIN32
        {
            DWORD tv = 8000;
            ::setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *) &tv, sizeof(tv));
            ::setsockopt(m_sock, SOL_SOCKET, SO_SNDTIMEO, (const char *) &tv, sizeof(tv));
            int one = 1;
            ::setsockopt(m_sock, IPPROTO_TCP, TCP_NODELAY, (const char *) &one, sizeof(one));
        }
#else
        {
            struct timeval tv;
            tv.tv_sec = 8; tv.tv_usec = 0;
            ::setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            ::setsockopt(m_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            int one = 1;
            ::setsockopt(m_sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        }
#endif

        SSL_library_init();
        SSL_load_error_strings();
        m_ctx = SSL_CTX_new(TLS_client_method());
        if (!m_ctx) {
            err = "SSL_CTX_new failed";
            return false;
        }
        SSL_CTX_set_verify(m_ctx, SSL_VERIFY_NONE, nullptr);
        m_ssl = SSL_new(m_ctx);
        if (!m_ssl) {
            err = "SSL_new failed";
            return false;
        }
        SSL_set_fd(m_ssl, (int) m_sock);
        SSL_set_tlsext_host_name(m_ssl, host.c_str());
        if (SSL_connect(m_ssl) != 1) {
            err = "TLS handshake failed";
            return false;
        }

        // ---- MQTT CONNECT
        std::string payload;
        payload += mqtt_str(client_id);
        payload += mqtt_str(user);
        payload += mqtt_str(pass);
        std::string var;
        var += mqtt_str("MQTT");     // protocol name
        var += (char) 4;             // level 3.1.1
        var += (char) 0xC2;          // user + pass + clean session
        var += (char) 0; var += (char) 60;   // keepalive 60s
        std::string body = var + payload;
        std::string pkt;
        pkt += (char) 0x10;
        pkt += mqtt_remaining(body.size());
        pkt += body;
        if (!write_all(pkt)) {
            err = "mqtt CONNECT write failed";
            return false;
        }
        std::string resp;
        if (!read_packet(resp, 10)) {
            err = "mqtt CONNACK not received";
            return false;
        }
        if (resp.size() < 4 || (unsigned char) resp[3] != 0) {
            err = "mqtt CONNACK rejected (rc=" + std::to_string(resp.size() > 3 ? (int) (unsigned char) resp[3] : -1) + ")";
            return false;
        }
        return true;
    }

    bool publish(const std::string &topic, const std::string &payload, std::string &err)
    {
        std::string body = mqtt_str(topic) + payload;   // QoS 0
        std::string pkt;
        pkt += (char) 0x30;
        pkt += mqtt_remaining(body.size());
        pkt += body;
        if (!write_all(pkt)) {
            err = "mqtt PUBLISH write failed";
            return false;
        }
        return true;
    }

    // optionally collect a few report frames after publishing
    void drain(int ms, std::vector<std::string> *sink)
    {
        const ULONG start = now_ms();
        while (now_ms() - start < (ULONG) ms) {
            std::string pkt;
            if (!read_packet(pkt, 400))
                break;
            if (sink && pkt.size() > 2)
                sink->push_back(pkt.substr(2));
        }
    }

    void close()
    {
        if (m_ssl) { SSL_shutdown(m_ssl); SSL_free(m_ssl); m_ssl = nullptr; }
        if (m_ctx) { SSL_CTX_free(m_ctx); m_ctx = nullptr; }
        if (m_sock != DSH_INVALID_SOCKET) { dsh_closesocket(m_sock); m_sock = DSH_INVALID_SOCKET; }
    }

private:
    dsh_socket_t m_sock { DSH_INVALID_SOCKET };
    SSL_CTX     *m_ctx { nullptr };
    SSL         *m_ssl { nullptr };

    static ULONG now_ms()
    {
#ifdef _WIN32
        return ::GetTickCount();
#else
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        return (ULONG)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
    }

    static std::string mqtt_str(const std::string &s)
    {
        std::string o;
        o += (char) ((s.size() >> 8) & 0xFF);
        o += (char) (s.size() & 0xFF);
        o += s;
        return o;
    }

    static std::string mqtt_remaining(size_t n)
    {
        std::string o;
        do {
            unsigned char b = (unsigned char) (n % 128);
            n /= 128;
            if (n > 0)
                b |= 0x80;
            o += (char) b;
        } while (n > 0);
        return o;
    }

    bool write_all(const std::string &d)
    {
        size_t off = 0;
        while (off < d.size()) {
            int n = SSL_write(m_ssl, d.data() + off, (int) (d.size() - off));
            if (n <= 0)
                return false;
            off += (size_t) n;
        }
        return true;
    }

    bool read_exact(char *dst, size_t n, int timeout_ms)
    {
        // NOTE: do NOT gate SSL_read on select(): a TLS record may already be decrypted
        // into OpenSSL's internal buffer, in which case select() on the raw socket never
        // fires and we would time out with the data already available. Rely on the socket
        // receive timeout set at connect time instead.
        size_t got = 0;
        while (got < n) {
            int r = SSL_read(m_ssl, dst + got, (int) (n - got));
            if (r > 0) {
                got += (size_t) r;
                continue;
            }
            int e = SSL_get_error(m_ssl, r);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
                continue;
            return false;   // timeout (SO_RCVTIMEO) or hard error
        }
        return true;
    }

    bool read_packet(std::string &out, int timeout_ms)
    {
        unsigned char hdr = 0;
        if (!read_exact((char *) &hdr, 1, timeout_ms))
            return false;

        // remaining length: MQTT varint, 1..4 bytes. These bytes are part of the packet
        // and MUST be kept, otherwise the assembled packet is short by their count.
        std::string lenbytes;
        size_t rem = 0, mult = 1;
        for (int i = 0; i < 4; ++i) {
            unsigned char b = 0;
            if (!read_exact((char *) &b, 1, timeout_ms))
                return false;
            lenbytes.push_back((char) b);
            rem += (size_t) (b & 0x7F) * mult;
            if (!(b & 0x80))
                break;
            mult *= 128;
        }

        std::string body(rem, '\0');
        if (rem && !read_exact(&body[0], rem, timeout_ms))
            return false;

        out.clear();
        out.push_back((char) hdr);
        out += lenbytes;
        out += body;
        return true;
    }
};

// ---------------------------------------------------------------- public API

bool bambu_lan_sender_enabled()
{
    // default ON; can be disabled with app config "use_builtin_lan_sender"="false"
    return true;
}

BambuLanPrintResult bambu_lan_send_print(const BambuLanPrintRequest &req,
                                         const BambuLanProgressFn &progress)
{
    BambuLanPrintResult r;

    // Progress is split across the whole operation so the UI advances smoothly:
    //   0..5    reading/hashing the slice file
    //   5..45   uploading <name>.gcode.3mf
    //  45..55   uploading the bare <name>.3mf
    //  55..65   writing the .bbl sidecar
    //  65..95   MQTT connect + publish
    //  95..100  draining the printer's reply
    DshProgress prog;
    prog.fn = &progress;
    auto step = [&](int lo, int hi, const char *phase, int inner) -> bool {
        prog.lo    = lo;
        prog.hi    = hi;
        prog.phase = phase;
        return prog.report(inner);
    };

    if (req.dev_ip.empty() || req.access_code.empty()) {
        r.error = "missing printer IP or access code";
        return r;
    }

    step(0, 5, "hashing", 0);
    uint64_t fsize = 0;
    const std::string md5 = md5_upper_hex_of_file(req.file_path, fsize);
    if (md5.empty()) {
        r.error = "cannot read slice file: " + req.file_path;
        return r;
    }
    if (!step(0, 5, "hashing", 100)) {
        r.error = "canceled";
        return r;
    }

    FtpTarget ftp;
    ftp.ip   = req.dev_ip;
    ftp.code = req.access_code;

    const std::string base   = req.project_name.empty() ? "dsh_job" : req.project_name;
    const std::string remote = base + ".gcode.3mf";
    const std::string bare   = base + ".3mf";

    long code = 0;
    std::string err;
    // The printer's storage directories are recreated lazily; after a card format
    // /cache may be missing entirely, so make sure it exists before uploading.
    dsh_ftp_mkdir_once(ftp, "/cache");
    BOOST_LOG_TRIVIAL(info) << "bambu_lan_sender: uploading " << req.file_path
                            << " (" << fsize << " bytes, md5=" << md5 << ") to /cache/" << remote;
    // The printer resolves the job through the sidecar's "file path" entry and has been
    // observed to look for the bare "<name>.3mf" name, so publish the payload under BOTH
    // candidate names (this is what the working reference upload did).
    prog.lo = 5; prog.hi = 45; prog.phase = "uploading slice";
    if (!dsh_ftp_upload(ftp, remote, req.file_path, nullptr, code, err, &prog)) {
        r.error = prog.aborted ? "canceled" : ("FTPS upload of " + remote + " failed: " + err);
        r.ftp_http_code = code;
        return r;
    }
    prog.lo = 45; prog.hi = 55; prog.phase = "uploading slice (alt name)";
    if (!dsh_ftp_upload(ftp, bare, req.file_path, nullptr, code, err, &prog)) {
        r.error = prog.aborted ? "canceled" : ("FTPS upload of " + bare + " failed: " + err);
        r.ftp_http_code = code;
        return r;
    }
    r.ftp_http_code = code;

    // sidecar .bbl describing the job
    std::ostringstream bbl;
    bbl << "{\n"
        << "\t\"file path\":\t\"/sdcard/cache/" << json_escape(bare) << "\",\n"
        << "\t\"subtask_name\":\t\"" << json_escape(base) << "\",\n"
        << "\t\"subtask id\":\t\"0\",\n"
        << "\t\"file_size\":\t\"" << fsize << "\",\n"
        << "\t\"md5\":\t\"" << md5 << "\",\n"
        << "\t\"timelapse\":\t" << (req.timelapse ? "true" : "false") << ",\n"
        << "\t\"bed_leveling\":\t" << (req.bed_leveling ? "true" : "false") << ",\n"
        << "\t\"flow_cali\":\t" << (req.flow_cali ? "true" : "false") << ",\n"
        << "\t\"vibration_cali\":\t" << (req.vibration_cali ? "true" : "false") << ",\n"
        << "\t\"layer_inspect\":\t" << (req.layer_inspect ? "true" : "false") << ",\n"
        << "\t\"use ams\":\t" << (req.use_ams ? "true" : "false") << ",\n"
        << "\t\"xy_mech_mode_sweep\":\tfalse,\n"
        << "\t\"auto_recovery\":\ttrue,\n"
        << "\t\"manual_color_change\":\tfalse,\n"
        << "\t\"ams mapping\":\t" << (req.ams_mapping.empty() ? "[]" : req.ams_mapping) << "\n"
        << "}\n";
    const std::string bbl_text  = bbl.str();
    const std::string bbl_remote = "1_" + base + ".gcode.bbl";
    prog.lo = 55; prog.hi = 65; prog.phase = "writing job metadata";
    if (!dsh_ftp_upload(ftp, bbl_remote, std::string(), &bbl_text, code, err, &prog)) {
        r.error = prog.aborted ? "canceled" : ("FTPS upload of " + bbl_remote + " failed: " + err);
        return r;
    }
    r.remote_file = remote;
    if (!step(55, 65, "writing job metadata", 100)) {
        r.error = "canceled";
        return r;
    }

    // ---- publish the print command
    std::ostringstream cmd;
    cmd << "{\"print\":{\"command\":\"project_file\",\"sequence_id\":\"" << (now_seq()) << "\","
        << "\"use_ams\":" << (req.use_ams ? "true" : "false") << ","
        << "\"ams_mapping\":" << (req.ams_mapping.empty() ? "[]" : req.ams_mapping) << ","
        << "\"bed_type\":\"" << json_escape(req.bed_type) << "\","
        << "\"url\":\"file:///sdcard/cache/" << json_escape(remote) << "\","
        << "\"file\":\"/cache/" << json_escape(remote) << "\","
        << "\"param\":\"Metadata/plate_" << req.plate_index << ".gcode\","
        << "\"md5\":\"" << md5 << "\","
        << "\"profile_id\":\"0\",\"project_id\":\"0\","
        << "\"subtask_id\":\"0\",\"subtask_name\":\"" << json_escape(base) << "\",\"task_id\":\"0\","
        << "\"timelapse\":" << (req.timelapse ? "true" : "false") << ","
        << "\"bed_leveling\":" << (req.bed_leveling ? "true" : "false") << ","
        << "\"flow_cali\":" << (req.flow_cali ? "true" : "false") << ","
        << "\"layer_inspect\":" << (req.layer_inspect ? "true" : "false") << ","
        << "\"vibration_cali\":" << (req.vibration_cali ? "true" : "false") << "}}";
    const std::string payload = cmd.str();

    MqttTlsClient mqtt;
    std::string mqtt_err;
    step(65, 85, "connecting to printer", 0);
    BOOST_LOG_TRIVIAL(info) << "bambu_lan_sender: mqtt connect " << req.dev_ip << ":8883";
    if (!mqtt.connect(req.dev_ip, 8883, "bblp", req.access_code, "orcaslicer", mqtt_err)) {
        r.error = "MQTT connect failed: " + mqtt_err;
        return r;
    }
    if (!step(65, 85, "connecting to printer", 100)) {
        r.error = "canceled";
        mqtt.close();
        return r;
    }
    const std::string topic = "device/" + req.dev_id + "/request";
    step(85, 95, "starting print", 0);
    BOOST_LOG_TRIVIAL(info) << "bambu_lan_sender: publish " << payload;
    if (!mqtt.publish(topic, payload, mqtt_err)) {
        r.error = "MQTT publish failed: " + mqtt_err;
        mqtt.close();
        return r;
    }
    if (!step(85, 95, "starting print", 100)) {
        r.error = "canceled";
        mqtt.close();
        return r;
    }
    step(95, 100, "waiting for printer", 0);
    std::vector<std::string> replies;
    mqtt.drain(4000, &replies);
    mqtt.close();
    for (const std::string &m : replies)
        BOOST_LOG_TRIVIAL(info) << "bambu_lan_sender: report " << m.substr(0, 300);
    step(95, 100, "waiting for printer", 100);

    r.ok = true;
    return r;
}

} // namespace Slic3r
