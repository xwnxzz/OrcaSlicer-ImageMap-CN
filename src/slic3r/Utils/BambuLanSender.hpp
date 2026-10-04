#ifndef slic3r_BambuLanSender_hpp_
#define slic3r_BambuLanSender_hpp_

// DSH-PATCH: self-implemented Bambu LAN print sender.
//
// The bundled closed-source bambu_networking plug-in cannot complete a LAN print on
// firmware that enforces MQTT command verification, and it crashes the process when its
// file-transfer tunnel fails. This module bypasses the plug-in entirely:
//
//   1. FTPS  (implicit TLS, port 990, user "bblp" / access code) -> upload the sliced
//      .gcode.3mf into the printer's /cache directory via libcurl
//   2. FTPS  -> write the sidecar /cache/1_<name>.gcode.bbl job description
//   3. MQTT  (TLS, port 8883, user "bblp" / access code) -> publish the project_file
//      command on device/<serial>/request
//
// Verified end-to-end against a Bambu Lab P1S in LAN mode.

#include <functional>
#include <string>

namespace Slic3r {

// Progress reporting during the LAN send.
//   percent 0..100 across the whole send operation
//   phase   short human-readable label for the current step
//   detail  optional extra text (e.g. the remote file name)
// Return false to abort the send (used for user cancellation).
using BambuLanProgressFn = std::function<bool(int percent, const std::string &phase,
                                             const std::string &detail)>;

struct BambuLanPrintRequest
{
    std::string dev_ip;        // printer LAN address
    std::string access_code;   // LAN access code (also the FTPS/MQTT password)
    std::string dev_id;        // printer serial number, e.g. 01P00C5C1602733
    std::string file_path;     // local path of the sliced .gcode.3mf
    std::string project_name;  // ASCII-safe subtask / remote base name
    int         plate_index { 1 };
    std::string bed_type { "textured_plate" };
    bool        use_ams { true };
    std::string ams_mapping { "[0,1,2,3]" };
    bool        bed_leveling { true };
    bool        flow_cali { false };
    bool        vibration_cali { false };
    bool        layer_inspect { false };
    bool        timelapse { false };
};

struct BambuLanPrintResult
{
    bool        ok { false };
    std::string error;        // human readable failure reason
    long        ftp_http_code { 0 };
    std::string remote_file;  // remote path that was uploaded
};

// Returns true when the built-in sender is enabled (app config "use_builtin_lan_sender").
bool bambu_lan_sender_enabled();

// Performs the whole upload + publish sequence. Blocking; call from a worker thread.
// progress may be empty; when supplied it is invoked repeatedly (coarsely rate-limited)
// with a monotonic percentage so the UI can show live progress.
BambuLanPrintResult bambu_lan_send_print(const BambuLanPrintRequest &req,
                                         const BambuLanProgressFn &progress = nullptr);

} // namespace Slic3r

#endif
