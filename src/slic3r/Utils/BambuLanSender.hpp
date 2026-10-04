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

#include <string>

namespace Slic3r {

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
BambuLanPrintResult bambu_lan_send_print(const BambuLanPrintRequest &req);

} // namespace Slic3r

#endif
