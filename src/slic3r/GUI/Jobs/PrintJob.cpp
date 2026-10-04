#include "PrintJob.hpp"
#include "libslic3r/MTUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/format.hpp"
#include "bambu_networking.hpp"

#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceCore/DevUtil.h"

#include "slic3r/Utils/FileTransferUtils.hpp"
#include "slic3r/Utils/BBLNetworkPlugin.hpp"
// DSH-PATCH: self-implemented LAN print sender (bypasses the closed-source plug-in)
#include "slic3r/Utils/BambuLanSender.hpp"

namespace Slic3r {
namespace GUI {

// DSH-PATCH: the ft_* module may legitimately not be initialised; diagnostics must
// never turn into a new crash site.
static std::string ft_abi_status_str()
{
    try {
        const FileTransferModule &m = module();
        const std::string ver = std::to_string(m.plugin_abi());
        std::string out = m.abi_ok() ? "true" : "false";
        out += "(plugin=";
        out += ver;
        out += ")";
        return out;
    } catch (...) {
        return "unavailable";
    }
}

// DSH-PATCH: the closed-source plug-in passes a null tunnel handle into
// ft_tunnel_sync_connect INSIDE its own LAN send functions (bambu_network_start_local_print,
// bambu_network_start_local_print_with_record, bambu_network_start_send_gcode_to_sdcard).
// That dereference is an ACCESS_VIOLATION we cannot catch or patch from here, so we must
// never hand it a situation it cannot handle: create the tunnel ourselves first, and if the
// handle comes back null, refuse to call the plug-in at all and report the real reason.
static bool lan_send_tunnel_viable(const std::string &dev_ip, const std::string &access_code,
                                   const std::string &dev_id)
{
    if (dev_ip.empty() || access_code.empty()) {
        BOOST_LOG_TRIVIAL(error) << "print_job: refusing plug-in LAN send - missing IP or access code"
                                 << " (dev_ip=\"" << dev_ip << "\", access_code_len=" << access_code.size()
                                 << ", dev_id=" << dev_id << ")";
        return false;
    }
    try {
        const std::string url = "bambu:///local/" + dev_ip + "?port=6000&user=bblp&passwd=" + access_code;
        std::unique_ptr<FileTransferTunnel> probe = std::make_unique<FileTransferTunnel>(module(), url);
        const bool viable = probe->sync_start_connect();
        BOOST_LOG_TRIVIAL(error) << "print_job: LAN tunnel pre-check dev_ip=" << dev_ip
                                 << ", dev_id=" << dev_id
                                 << ", handle=" << (probe->native() ? "valid" : "NULL")
                                 << ", sync_connect=" << (viable ? "ok" : "failed")
                                 << ", ft_abi=" << ft_abi_status_str()
                                 << " => " << (viable ? "plug-in LAN send allowed" : "plug-in LAN send REFUSED (would crash)");
        return viable;
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "print_job: LAN tunnel pre-check threw (" << e.what()
                                 << ") => plug-in LAN send REFUSED (would crash)";
        return false;
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "print_job: LAN tunnel pre-check threw (unknown) => plug-in LAN send REFUSED";
        return false;
    }
}

// Result code reported when we refuse to enter the plug-in because its own tunnel
// setup would hand a null handle to ft_tunnel_sync_connect.
static int lan_send_refused_result() { return BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED; }

static auto check_gcode_failed_str      = _u8L("Abnormal print file data. Please slice again.");
static auto     printjob_cancel_str         = _u8L("Task canceled.");
static auto     timeout_to_upload_str       = _u8L("Upload task timed out. Please check the network status and try again.");
static auto     failed_in_cloud_service_str = _u8L("Cloud service connection failed. Please try again.");
static auto     file_is_not_exists_str      = _u8L("Print file not found. Please slice again.");
static auto file_over_size_str = _u8L("The print file exceeds the maximum allowable size (1GB). Please simplify the model and slice again.");
static auto print_canceled_str    = _u8L("Task canceled.");
static auto send_print_failed_str = _u8L("Failed to send the print job. Please try again.");
static auto upload_ftp_failed_str = _u8L("Failed to upload file to ftp. Please try again.");

static auto     desc_network_error          = _u8L("Check the current status of the bambu server by clicking on the link above.");
static auto     desc_file_too_large         = _u8L("The size of the print file is too large. Please adjust the file size and try again.");
static auto     desc_fail_not_exist         = _u8L("Print file not found, please slice it again and send it for printing.");

static auto desc_upload_ftp_failed      = _u8L("Failed to upload print file to FTP. Please check the network status and try again.");

static auto sending_over_lan_str        = _u8L("Sending print job over LAN");
static auto sending_over_cloud_str      = _u8L("Sending print job through cloud service");

static wxString wait_sending_finish         = _L("Print task sending times out.");
//static wxString desc_wait_sending_finish    = _L("The printer timed out while receiving a print job. Please check if the network is functioning properly and send the print again.");
//static wxString desc_wait_sending_finish    = _L("The printer timed out while receiving a print job. Please check if the network is functioning properly.");

PrintJob::PrintJob(std::string dev_id)
: m_plater{wxGetApp().plater()},
    m_dev_id(dev_id),
    m_is_calibration_task(false)
{
    m_print_job_completed_id = m_plater->get_print_finished_event();
}

void PrintJob::prepare()
{
    if (job_data.is_from_plater)
        m_plater->get_print_job_data(&job_data);
    std::string temp_file = Slic3r::resources_dir() + "/check_access_code.txt";
    auto check_access_code_path = temp_file.c_str();
    BOOST_LOG_TRIVIAL(trace) << "sned_job: check_access_code_path = " << check_access_code_path;
    job_data._temp_path = fs::path(check_access_code_path);
}

void PrintJob::on_success(std::function<void()> success)
{
    m_success_fun = success;
}

std::string PrintJob::truncate_string(const std::string& str, size_t maxLength)
{
    if (str.length() <= maxLength)
    {
        return str;
    }

    wxString local_str = wxString::FromUTF8(str);
    wxString truncatedStr;

    for (auto i = 1; i < local_str.Length(); i++) {
        wxString tagStr = local_str.Mid(0, i);
        if (tagStr.ToUTF8().length() >= maxLength) {
            truncatedStr = local_str.Mid(0, i - 1);
            break;
        }
    }
    return truncatedStr.utf8_string();
}


wxString PrintJob::get_http_error_msg(unsigned int status, std::string body)
{
    try {
        int code = 0;
        std::string error;
        std::string message;
        wxString result;
        if (status >= 400 && status < 500)
            try {
            json j = json::parse(body);
            if (j.contains("code")) {
                if (!j["code"].is_null())
                    code = j["code"].get<int>();
            }
            if (j.contains("error")) {
                if (!j["error"].is_null())
                    error = j["error"].get<std::string>();
            }
            if (j.contains("message")) {
                if (!j["message"].is_null())
                    message = j["message"].get<std::string>();
            }
        }
        catch (...) {
            ;
        }
        else if (status == 503) {
            return _L("Service Unavailable");
        }
        else {
            wxString unkown_text = _L("Unknown Error.");
            unkown_text += wxString::Format("status=%u, body=%s", status, body);
            BOOST_LOG_TRIVIAL(error) << "http_error: status=" << status << ", code=" << code << ", error=" << error;
            return unkown_text;
        }

        BOOST_LOG_TRIVIAL(error) << "http_error: status=" << status << ", code=" << code << ", error=" << error;

        result = wxString::Format("code=%u, error=%s", code, from_u8(error));
        return result;
    } catch(...) {
        ;
    }
    return wxEmptyString;
}

void PrintJob::process(Ctl &ctl)
{
    /* display info */
    std::string msg;
    wxString error_str;
    int curr_percent = 10;
    NetworkAgent* m_agent = wxGetApp().getAgent();
    AppConfig* config = wxGetApp().app_config;

    if (this->connection_type == "lan") {
        msg = _u8L("Sending print job over LAN");
    }
    else {
        msg = _u8L("Sending print job through cloud service");
    }

    ctl.update_status(0, msg);
    ctl.call_on_main_thread([this] { prepare(); }).wait();

    int result = -1;
    std::string http_body;

    int total_plate_num = plate_data.plate_count;
    if (!plate_data.is_valid) {
        total_plate_num =  m_plater->get_partplate_list().get_plate_count();
        PartPlate *plate = m_plater->get_partplate_list().get_plate(job_data.plate_idx);
        if (plate == nullptr) {
            plate = m_plater->get_partplate_list().get_curr_plate();
            if (plate == nullptr) return;
        }

        /* check gcode is valid */
        if (!plate->is_valid_gcode_file() && m_print_type == "from_normal") {
            ctl.update_status(curr_percent, check_gcode_failed_str);
            return;
        }

        if (ctl.was_canceled()) {
            ctl.update_status(curr_percent, printjob_cancel_str);
            return;
        }
    }

    m_project_name = truncate_string(m_project_name, 100);
    int curr_plate_idx = 0;

    if (m_print_type == "from_normal") {
        if (plate_data.is_valid)
            curr_plate_idx = plate_data.cur_plate_index;
        if (job_data.plate_idx >= 0)
            curr_plate_idx = job_data.plate_idx + 1;
        else if (job_data.plate_idx == PLATE_CURRENT_IDX)
            curr_plate_idx = m_plater->get_partplate_list().get_curr_plate_index() + 1;
        else if (job_data.plate_idx == PLATE_ALL_IDX)
            curr_plate_idx = m_plater->get_partplate_list().get_curr_plate_index() + 1;
        else
            curr_plate_idx = m_plater->get_partplate_list().get_curr_plate_index() + 1;
    }
    else if(m_print_type == "from_sdcard_view") {
        curr_plate_idx = m_print_from_sdc_plate_idx;
    }

    PartPlate* curr_plate = m_plater->get_partplate_list().get_curr_plate();
    if (curr_plate) {
        this->task_bed_type = bed_type_to_gcode_string(plate_data.is_valid ? plate_data.bed_type : curr_plate->get_bed_type(true));
    }

    PrintParams params;

    // local print access
    params.dev_ip = m_dev_ip;
    params.use_ssl_for_ftp  = m_local_use_ssl_for_ftp;
    params.use_ssl_for_mqtt  = m_local_use_ssl;
    params.username = "bblp";
    params.password = m_access_code;

    // check access code and ip address
    if (this->connection_type == "lan" && m_print_type == "from_normal") {
        bool emmc_ok = false;
        bool ftp_ok = false;
        BOOST_LOG_TRIVIAL(info) << "print_job: LAN pre-flight, could_emmc_print=" << could_emmc_print
                                << ", has_sdcard=" << has_sdcard
                                << ", cloud_print_only=" << cloud_print_only
                                << ", sdcard_state=" << (int) sdcard_state
                                << ", dev_ip=" << m_dev_ip
                                << ", password_len=" << m_access_code.size()
                                << ", ft_abi_ok=" << ft_abi_status_str();
        // DSH-PATCH: the plug-in's own bambu_network_start_send_gcode_to_sdcard was
        // observed to call ft_tunnel_sync_connect internally and crash the process.
        // That crash site is inside the closed-source plug-in and therefore cannot be
        // guarded from here - the only reliable fix is to not run the pre-flight.
        // Set app config "skip_lan_print_preflight" to "false" to restore the old behaviour.
        const std::string preflight_cfg = wxGetApp().app_config->get("skip_lan_print_preflight");
        const bool skip_preflight = preflight_cfg.empty() || preflight_cfg != "false";
        BOOST_LOG_TRIVIAL(info) << "print_job: skip_lan_print_preflight config=\"" << preflight_cfg
                                << "\" => " << (skip_preflight ? "skipping pre-flight" : "running pre-flight");

        if (!skip_preflight && could_emmc_print) {
            std::string devIP = m_dev_ip;
            std::string accessCode = m_access_code;
            std::string url = "bambu:///local/" + devIP + "?port=6000&user=" + "bblp" + "&passwd=" + accessCode;
            // DSH-PATCH: this tunnel probe used to crash the process with an
            // ACCESS_VIOLATION inside the closed-source plug-in. It is only a
            // pre-flight check, so a failure here must never abort the send.
            try {
                std::unique_ptr<FileTransferTunnel> tunnel = std::make_unique<FileTransferTunnel>(module(), url);
                // DSH-PATCH: a valid tunnel handle is required before the plug-in may be
                // called; a null handle used to reach ft_tunnel_sync_connect and crash.
                BOOST_LOG_TRIVIAL(info) << "print_job: emmc tunnel created, handle="
                                        << (tunnel->native() ? "valid" : "NULL") << ", url=" << url;
                emmc_ok = tunnel->sync_start_connect();
                BOOST_LOG_TRIVIAL(info) << "print_job: emmc tunnel sync_start_connect => " << (emmc_ok ? "ok" : "failed");
            } catch (const std::exception &e) {
                BOOST_LOG_TRIVIAL(warning) << "print_job: emmc tunnel probe failed: " << e.what();
                emmc_ok = false;
            } catch (...) {
                BOOST_LOG_TRIVIAL(warning) << "print_job: emmc tunnel probe failed (unknown exception)";
                emmc_ok = false;
            }
        }
        if (!skip_preflight && !emmc_ok) {
            params.dev_id = m_dev_id;
            params.project_name = "verify_job";
            params.filename = job_data._temp_path.string();
            params.connection_type = this->connection_type;

            // DSH-PATCH: treat any return value from the verification upload as
            // meaningful only when it is non-negative. An unmapped negative code
            // (e.g. -26) used to abort the LAN send and surface a generic error.
            result = m_agent->start_send_gcode_to_sdcard(params, nullptr, nullptr, nullptr);

            ftp_ok = result == 0;
            if (!ftp_ok) {
                BOOST_LOG_TRIVIAL(warning) << "print_job: LAN verification returned " << result
                                           << "; continuing with the real send attempt";
            }
        }
        if (!emmc_ok && !ftp_ok) {
            bool legacy_mode = BBLNetworkPlugin::instance().use_legacy_network();
            // DSH-PATCH: with the pre-flight skipped this block is expected to be
            // reached; it is no longer an error, just a note. BOOST_LOG_TRIVIAL needs a
            // literal severity, so the two cases are separate statements.
            if (skip_preflight) {
                BOOST_LOG_TRIVIAL(info) << "LAN pre-flight skipped (configuration), proceeding to send:"
                    << " emmc_ok=" << emmc_ok
                    << ", ftp_ok=" << ftp_ok
                    << ", ftp_result=" << result
                    << ", dev_ip=" << m_dev_ip
                    << ", dev_id=" << m_dev_id
                    << ", password_length=" << m_access_code.size()
                    << ", legacy_mode=" << (legacy_mode ? "true" : "false");
            } else {
                BOOST_LOG_TRIVIAL(error) << "LAN connection verification failed:"
                    << " emmc_ok=" << emmc_ok
                    << ", ftp_ok=" << ftp_ok
                    << ", ftp_result=" << result
                    << ", dev_ip=" << m_dev_ip
                    << ", dev_id=" << m_dev_id
                    << ", password_length=" << m_access_code.size()
                    << ", legacy_mode=" << (legacy_mode ? "true" : "false");
            }
            // DSH-PATCH: verification is advisory. Do not give up here - fall
            // through to the real send so the user gets a genuine result.
            result = 0;
            BOOST_LOG_TRIVIAL(warning) << "print_job: proceeding to send";
        }

        params.project_name = "";
        params.filename = "";
    }

    params.dev_id               = m_dev_id;
    params.ftp_folder           = m_ftp_folder;
    params.filename             = job_data._3mf_path.string();
    params.config_filename      = job_data._3mf_config_path.string();
    params.plate_index          = curr_plate_idx;
    params.task_bed_leveling    = this->task_bed_leveling;
    params.task_flow_cali       = this->task_flow_cali;
    params.task_vibration_cali  = this->task_vibration_cali;
    params.task_layer_inspect   = this->task_layer_inspect;
    params.task_record_timelapse= this->task_record_timelapse;
    params.nozzle_mapping       = this->task_nozzle_mapping;
    params.ams_mapping          = this->task_ams_mapping;
    params.ams_mapping2         = this->task_ams_mapping2;
    params.ams_mapping_info     = this->task_ams_mapping_info;
    params.nozzles_info         = this->task_nozzles_info;
    params.connection_type      = this->connection_type;
    params.task_use_ams         = this->task_use_ams;
    params.task_bed_type        = this->task_bed_type;
    params.print_type           = this->m_print_type;
    params.auto_bed_leveling    = this->auto_bed_leveling;
    params.auto_flow_cali       = this->auto_flow_cali;
    params.auto_offset_cali     = this->auto_offset_cali;
    params.task_ext_change_assist = this->task_ext_change_assist;
    params.try_emmc_print         = this->could_emmc_print;

    if (m_print_type == "from_sdcard_view") {
        params.dst_file = m_dst_path;
    }

    if (wxGetApp().model().model_info && wxGetApp().model().model_info.get()) {
        ModelInfo* model_info = wxGetApp().model().model_info.get();
        auto origin_profile_id = model_info->metadata_items.find(BBL_DESIGNER_PROFILE_ID_TAG);
        if (origin_profile_id != model_info->metadata_items.end()) {
            try {
                params.origin_profile_id    = stoi(origin_profile_id->second.c_str());
            }
            catch(...) {}
        }
        auto origin_model_id = model_info->metadata_items.find(BBL_DESIGNER_MODEL_ID_TAG);
        if (origin_model_id != model_info->metadata_items.end()) {
            try {
                params.origin_model_id = origin_model_id->second;
            }
            catch(...) {}
        }

        auto profile_name = model_info->metadata_items.find(BBL_DESIGNER_PROFILE_TITLE_TAG);
        if (profile_name != model_info->metadata_items.end()) {
            try {
                params.preset_name = profile_name->second;
            }
            catch (...) {}
        }

         if (m_print_type != "from_sdcard_view") {
            auto model_name = model_info->metadata_items.find(BBL_DESIGNER_MODEL_TITLE_TAG);
            if (model_name != model_info->metadata_items.end()) {
                try {
                    std::string mall_model_name = model_name->second;
                    std::replace(mall_model_name.begin(), mall_model_name.end(), ' ', '_');
                    const char *unusable_symbols = "<>[]:/\\|?*\" ";
                    for (const char *symbol = unusable_symbols; *symbol != '\0'; ++symbol) { std::replace(mall_model_name.begin(), mall_model_name.end(), *symbol, '_'); }

                    std::regex pattern("_+");
                    params.project_name = std::regex_replace(mall_model_name, pattern, "_");
                    params.project_name = truncate_string(params.project_name, 100);
                } catch (...) {}
            }
        }
    }

    params.stl_design_id = 0;
    if (!wxGetApp().model().stl_design_id.empty()) {

        auto country_code = wxGetApp().app_config->get_country_code();
        bool match_code = false;

        if (wxGetApp().model().stl_design_country == "DEV" && (country_code == "ENV_CN_DEV" || country_code == "NEW_ENV_DEV_HOST")) {
            match_code = true;
        }

        if (wxGetApp().model().stl_design_country == "QA" && (country_code == "ENV_CN_QA" || country_code == "NEW_ENV_QAT_HOST")) {
            match_code = true;
        }

        if (wxGetApp().model().stl_design_country == "CN_PRE" && (country_code == "ENV_CN_PRE" || country_code == "NEW_ENV_PRE_HOST")) {
            match_code = true;
        }

        if (wxGetApp().model().stl_design_country == "US_PRE" && country_code == "ENV_US_PRE") {
            match_code = true;
        }

        if (country_code == wxGetApp().model().stl_design_country) {
            match_code = true;
        }

        if (match_code) {
            int stl_design_id = 0;
            try {
                stl_design_id = std::stoi(wxGetApp().model().stl_design_id);
            }
            catch (const std::exception&) {
                stl_design_id = 0;
            }
            params.stl_design_id = stl_design_id;
        }
    }

    if (params.preset_name.empty() && m_print_type == "from_normal") { params.preset_name = wxString::Format("%s_plate_%d", m_project_name, curr_plate_idx).ToStdString(); }
    if (params.project_name.empty()) {params.project_name = m_project_name;}

    if (m_is_calibration_task) {
        params.project_name = m_project_name;
        params.origin_model_id = "";
    }

    wxString error_text;
    std::string msg_text;


    const int StagePercentPoint[(int)PrintingStageFinished + 1] = {
        20,     // PrintingStageCreate
        30,     // PrintingStageUpload
        70,     // PrintingStageWaiting
        75,     // PrintingStageRecord
        97,     // PrintingStageSending
        100,    // PrintingStageFinished
        100     // PrintingStageFinished
    };

    bool is_try_lan_mode = false;
    bool is_try_lan_mode_failed = false;

    auto update_fn = [this, &ctl,
        &is_try_lan_mode,
        &is_try_lan_mode_failed,
        &msg,
        &error_str,
        &curr_percent,
        &error_text,
        StagePercentPoint
    ](int stage, int code, std::string info) {

                        if (stage == SendingPrintJobStage::PrintingStageCreate && !is_try_lan_mode_failed) {
                            if (this->connection_type == "lan") {
                                msg = _u8L("Sending print job over LAN");
                            } else {
                                msg = _u8L("Sending print job through cloud service");
                            }
                        }
                        else if (stage == SendingPrintJobStage::PrintingStageUpload && !is_try_lan_mode_failed) {
                            if (code >= 0 && code <= 100 && !info.empty()) {
                                if (this->connection_type == "lan") {
                                    msg = _u8L("Sending print job over LAN");
                                } else {
                                    msg = _u8L("Sending print job through cloud service");
                                }
                                msg += format("(%s)", info);
                            }
                        }
                        else if (stage == SendingPrintJobStage::PrintingStageWaiting) {
                            if (this->connection_type == "lan") {
                                msg = _u8L("Sending print job over LAN");
                            } else {
                                msg = _u8L("Sending print job through cloud service");
                            }
                        }
                        else  if (stage == SendingPrintJobStage::PrintingStageRecord && !is_try_lan_mode) {
                            msg = _u8L("Sending print configuration");
                        }
                        else if (stage == SendingPrintJobStage::PrintingStageSending && !is_try_lan_mode) {
                            if (this->connection_type == "lan") {
                                msg = _u8L("Sending print job over LAN");
                            } else {
                                msg = _u8L("Sending print job through cloud service");
                            }
                        }
                        else if (stage == SendingPrintJobStage::PrintingStageFinished) {
                            msg = format(_u8L("Successfully sent. Will automatically jump to the device page in %ss"), info);
                            if (m_print_job_completed_id == wxGetApp().plater()->get_send_calibration_finished_event()) {
                                msg = format(_u8L("Successfully sent. Will automatically jump to the next page in %ss"), info);
                            }
                            ctl.clear_percent();
                        } else {
                            if (this->connection_type == "lan") {
                                msg = _u8L("Sending print job over LAN");
                            } else {
                                msg = _u8L("Sending print job through cloud service");
                            }
                        }

                        // update current percnet
                        if (stage >= 0 && stage <= (int) PrintingStageFinished) {
                            curr_percent = StagePercentPoint[stage];
                            if ((stage == SendingPrintJobStage::PrintingStageUpload
                                || stage == SendingPrintJobStage::PrintingStageRecord)
                                && (code > 0 && code <= 100)) {
                                curr_percent = (StagePercentPoint[stage + 1] - StagePercentPoint[stage]) * code / 100 + StagePercentPoint[stage];
                            }
                        }

                        //get errors
                        if (code > 100 || code < 0 || stage == SendingPrintJobStage::PrintingStageERROR) {
                            if (code == BAMBU_NETWORK_ERR_PRINT_WR_FILE_OVER_SIZE || code == BAMBU_NETWORK_ERR_PRINT_SP_FILE_OVER_SIZE) {
                                m_plater->update_print_error_info(code, desc_file_too_large, info);
                            }else if (code == BAMBU_NETWORK_ERR_PRINT_WR_FILE_NOT_EXIST || code == BAMBU_NETWORK_ERR_PRINT_SP_FILE_NOT_EXIST){
                                m_plater->update_print_error_info(code, desc_fail_not_exist, info);
                            }else if (code == BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED || code == BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED) {
                                m_plater->update_print_error_info(code, desc_upload_ftp_failed, info);
                            }else {
                                m_plater->update_print_error_info(code, desc_network_error, info);
                            }
                        }
                        else {
                             ctl.update_status(curr_percent, msg);
                        }
                    };

    auto cancel_fn = [&ctl]() {
            return ctl.was_canceled();
        };


    DeviceManager* dev = wxGetApp().getDeviceManager();
    MachineObject* obj = dev->get_selected_machine();

    auto wait_fn = [this, curr_percent, &obj](int state, std::string job_info) {
            BOOST_LOG_TRIVIAL(info) << "print_job: get_job_info = " << job_info;

            if (!obj->is_support_wait_sending_finish) {
                return true;
            }

            std::string curr_job_id;
            json job_info_j;
            try {
                std::ignore = job_info_j.parse(job_info);
                if (job_info_j.contains("job_id")) {
                    curr_job_id = DevJsonValParser::get_longlong_val(job_info_j["job_id"]);
                }
                BOOST_LOG_TRIVIAL(trace) << "print_job: curr_obj_id=" << curr_job_id;

            } catch(...) {
                ;
            }

            if (obj) {
                int time_out = 0;
                while (time_out < PRINT_JOB_SENDING_TIMEOUT) {
                    BOOST_LOG_TRIVIAL(trace) << "print_job: obj job_id = " << obj->job_id_;
                    if (!obj->job_id_.empty() && obj->job_id_.compare(curr_job_id) == 0) {
                        BOOST_LOG_TRIVIAL(info) << "print_job: got job_id = " << obj->job_id_ << ", time_out=" << time_out;
                        return true;
                    }
                    if (obj->is_in_printing_status(obj->print_status)) {
                        BOOST_LOG_TRIVIAL(info) << "print_job: printer has enter printing status, s = " << obj->print_status;
                        return true;
                    }
                    time_out++;
                    boost::this_thread::sleep_for(boost::chrono::milliseconds(1000));
                }
                //this->update_status(curr_percent, _L("Print task sending times out."));
                //m_plater->update_print_error_info(BAMBU_NETWORK_ERR_TIMEOUT, wait_sending_finish.ToStdString(), desc_wait_sending_finish.ToStdString());
                BOOST_LOG_TRIVIAL(info) << "print_job: timeout, cancel the job" << obj->job_id_;
                /* handle tiemout */
                //obj->command_task_cancel(curr_job_id);
                //return false;
                return true;
            }
            BOOST_LOG_TRIVIAL(info) << "print_job: obj is null";
            return true;
    };

    // DSH-PATCH: every LAN send entry point except the record-based one reaches
    // ft_tunnel_sync_connect inside the closed-source plug-in and dies there when the
    // tunnel is not usable. Only start_local_print_with_record (FTPS on 990) is left
    // ungated; everything else is gated on a real tunnel probe.
    const bool probe_needed = could_emmc_print
                           // the storage-state switch below calls start_local_print
                           || sdcard_state == DevStorage::SdcardState::HAS_SDCARD_NORMAL
                           || sdcard_state == DevStorage::SdcardState::HAS_SDCARD_ABNORMAL;
    bool emmc_send_allowed = true;
    if (probe_needed) {
        emmc_send_allowed = lan_send_tunnel_viable(params.dev_ip, params.password, params.dev_id);
    }
    BOOST_LOG_TRIVIAL(error) << "print_job: LAN send dispatch, emmc_send_allowed=" << emmc_send_allowed
                             << ", probe_needed=" << probe_needed
                             << ", print_type=" << m_print_type
                             << ", connection_type=" << params.connection_type
                             << ", could_emmc_print=" << could_emmc_print
                             << ", has_sdcard=" << has_sdcard
                             << ", sdcard_state=" << (int) sdcard_state
                             << ", dev_ip=" << params.dev_ip
                             << ", password_len=" << params.password.size()
                             << ", cloud_print_only=" << this->cloud_print_only;

    // DSH-PATCH: self-implemented LAN send path.
    //
    // The closed-source bambu_networking plug-in cannot complete a LAN print on firmware
    // that enforces MQTT command verification (it answers with HMS 0500-0500-0001-0007),
    // and its file-transfer tunnel crashes the process when the tunnel is unusable.
    // BambuLanSender performs the same job with no plug-in involvement:
    //   FTPS on 990 (libcurl, implicit TLS) uploads <name>.gcode.3mf into /cache,
    //   plus a sidecar 1_<name>.gcode.bbl, then one MQTT project_file on 8883.
    // Set app config "use_builtin_lan_sender" to "false" to restore the old dispatch.
    {
        const std::string builtin_cfg = wxGetApp().app_config->get("use_builtin_lan_sender");
        const bool builtin_enabled = builtin_cfg.empty() || builtin_cfg != "false";

        BOOST_LOG_TRIVIAL(error) << "print_job: builtin_lan_sender enabled=" << builtin_enabled
                                 << ", cfg=\"" << builtin_cfg << "\""
                                 << ", connection_type=" << params.connection_type
                                 << ", print_type=" << m_print_type;

        if (builtin_enabled
            && params.connection_type == "lan"
            && m_print_type == "from_normal"
            && !params.dev_ip.empty()
            && !params.password.empty()
            && !params.filename.empty()) {

            // printer-side file names must be ASCII and unique per job
            std::string ascii;
            for (char ch : m_project_name) {
                if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
                    || ch == '_' || ch == '-')
                    ascii += ch;
                else if (ch == ' ' || ch == '.')
                    ascii += '_';
            }
            if (ascii.size() > 48)
                ascii.resize(48);
            if (ascii.empty())
                ascii = "job";
            const std::string job_name = ascii + "_" + std::to_string((long) ::time(nullptr));

            BambuLanPrintRequest req;
            req.dev_ip       = params.dev_ip;
            req.access_code  = params.password;
            req.dev_id       = params.dev_id;
            req.file_path    = params.filename;
            req.project_name = job_name;
            req.plate_index  = params.plate_index;
            req.use_ams      = !this->task_ams_mapping.empty();
            req.bed_type     = this->task_bed_type.empty() ? "textured_plate" : this->task_bed_type;
            req.ams_mapping  = this->task_ams_mapping.empty() ? "[]" : this->task_ams_mapping;
            req.bed_leveling    = params.task_bed_leveling;
            req.flow_cali       = params.task_flow_cali;
            req.vibration_cali  = params.task_vibration_cali;
            req.layer_inspect   = params.task_layer_inspect;
            req.timelapse       = params.task_record_timelapse;

            BOOST_LOG_TRIVIAL(error) << "print_job: builtin LAN send, ip=" << req.dev_ip
                                     << ", dev_id=" << req.dev_id
                                     << ", plate=" << req.plate_index
                                     << ", ams=" << req.ams_mapping
                                     << ", bed_type=" << req.bed_type
                                     << ", name=" << req.project_name
                                     << ", file=" << req.file_path;

            ctl.update_status(curr_percent, _u8L("Sending print job over LAN"));
            is_try_lan_mode = true;

            const BambuLanPrintResult lr = bambu_lan_send_print(req);
            if (lr.ok) {
                BOOST_LOG_TRIVIAL(error) << "print_job: builtin LAN send OK, remote=" << lr.remote_file;
                result = 0;
            } else {
                BOOST_LOG_TRIVIAL(error) << "print_job: builtin LAN send FAILED: " << lr.error
                                         << " (ftp_code=" << lr.ftp_http_code << ")";
                error_text = wxString::Format(_L("Access code:%s IP address:%s"),
                                             params.password, params.dev_ip);
                result = -1;   // fall through to the plug-in path below
            }
            if (result == 0)
                goto dsh_builtin_lan_done;
        }
    }

    if (m_print_type == "from_sdcard_view") {
        BOOST_LOG_TRIVIAL(info) << "print_job: try to send with cloud, model is sdcard view";
        ctl.update_status(curr_percent, _u8L("Sending print job through cloud service"));
        result = m_agent->start_sdcard_print(params, update_fn, cancel_fn);
    } else if (params.connection_type != "lan") {
        if (params.dev_ip.empty())
            params.comments = "no_ip";
        else if (this->cloud_print_only)
            params.comments = "low_version";
        else if (!this->has_sdcard)
            params.comments = "no_sdcard";
        else if (params.password.empty())
            params.comments = "no_password";


        //use ftp only
        if (!wxGetApp().app_config->get("lan_mode_only").empty() && wxGetApp().app_config->get("lan_mode_only") == "1") {

            if (params.password.empty() || params.dev_ip.empty()) {
                error_text = wxString::Format(_L("Access code:%s IP address:%s"), params.password, params.dev_ip);
                result = BAMBU_NETWORK_ERR_FTP_UPLOAD_FAILED;
            }
            else {
                BOOST_LOG_TRIVIAL(info) << "print_job: use ftp send print only";
                ctl.update_status(curr_percent, _u8L("Sending print job over LAN"));
                is_try_lan_mode = true;
                // DSH-PATCH: standard LAN send uses FTPS on 990, not the port-6000 eMMC
                // tunnel, so it is deliberately NOT gated by the tunnel probe.
                result = m_agent->start_local_print_with_record(params, update_fn, cancel_fn, wait_fn);
                if (result < 0) {
                    error_text = wxString::Format(_L("Access code:%s IP address:%s"), params.password, params.dev_ip);
                    // try to send with cloud
                    BOOST_LOG_TRIVIAL(warning) << "print_job: use ftp send print failed";
                }
            }
        }
        else {
            if (!this->cloud_print_only
                && !params.password.empty()
                && !params.dev_ip.empty()
                && this->has_sdcard) {
                // try to send local with record
                BOOST_LOG_TRIVIAL(info) << "print_job: try to start local print with record";
                ctl.update_status(curr_percent, _u8L("Sending print job over LAN"));
                // DSH-PATCH: standard LAN send uses FTPS on 990, not the port-6000 eMMC
                // tunnel, so it is deliberately NOT gated by the tunnel probe.
                result = m_agent->start_local_print_with_record(params, update_fn, cancel_fn, wait_fn);
                if (result == 0) {
                    params.comments = "";
                }
                else if (result == BAMBU_NETWORK_ERR_PRINT_WR_UPLOAD_FTP_FAILED) {
                    params.comments = "upload_failed";
                }
                else {
                    params.comments = (boost::format("failed(%1%)") % result).str();
                }
                if (result < 0) {
                    is_try_lan_mode_failed = true;
                    // try to send with cloud
                    BOOST_LOG_TRIVIAL(warning) << "print_job: try to send with cloud";
                    ctl.update_status(curr_percent, _u8L("Sending print job through cloud service"));
                    result = m_agent->start_print(params, update_fn, cancel_fn, wait_fn);
                }
            }
            else {
                BOOST_LOG_TRIVIAL(info) << "print_job: send with cloud";
                ctl.update_status(curr_percent, _u8L("Sending print job through cloud service"));
                result = m_agent->start_print(params, update_fn, cancel_fn, wait_fn);
            }
        }
    } else {
        if (this->could_emmc_print) {
            ctl.update_status(curr_percent, _u8L("Sending print job over LAN"));
            // DSH-PATCH: eMMC storage send needs the port-6000 tunnel; refuse it when the
            // plug-in cannot build that tunnel (it would crash).
            if (emmc_send_allowed)
                result = m_agent->start_local_print(params, update_fn, cancel_fn);
            else
                result = lan_send_refused_result();
        } else {
            switch(this->sdcard_state) {
                case DevStorage::SdcardState::NO_SDCARD:
                    ctl.update_status(curr_percent, _u8L("A Storage needs to be inserted before printing via LAN."));
                    return;
                case DevStorage::SdcardState::HAS_SDCARD_ABNORMAL:
                    if(this->has_sdcard) {
                        // means the storage is abnormal but can be used option is enabled
                        ctl.update_status(curr_percent, _u8L("Sending print job over LAN, but the Storage in the printer is abnormal and print-issues may be caused by this."));
                        // DSH-PATCH: eMMC storage send needs the port-6000 tunnel; refuse it
                        // when the plug-in cannot build that tunnel (it would crash).
                        if (emmc_send_allowed)
                            result = m_agent->start_local_print(params, update_fn, cancel_fn);
                        else
                            result = lan_send_refused_result();
                        break;
                    }
                    ctl.update_status(curr_percent, _u8L("The Storage in the printer is abnormal. Please replace it with a normal Storage before sending print job to printer."));
                    return;
                case DevStorage::SdcardState::HAS_SDCARD_READONLY:
                    ctl.update_status(curr_percent, _u8L("The Storage in the printer is read-only. Please replace it with a normal Storage before sending print job to printer."));
                    return;
                case DevStorage::SdcardState::HAS_SDCARD_NORMAL:
                    ctl.update_status(curr_percent, _u8L("Sending print job over LAN"));
                    // DSH-PATCH: eMMC storage send needs the port-6000 tunnel; refuse it when
                    // the plug-in cannot build that tunnel (it would crash).
                    if (emmc_send_allowed)
                        result = m_agent->start_local_print(params, update_fn, cancel_fn);
                    else
                        result = lan_send_refused_result();
                    break;
                default:
                    ctl.update_status(curr_percent, _u8L("Encountered an unknown error with the Storage status. Please try again."));
                    return;
            }
        }
    }

    // DSH-PATCH: jump target for a successful built-in LAN send (skips the plug-in dispatch).
dsh_builtin_lan_done:
    if (result < 0) {
        curr_percent = -1;

        if (result == BAMBU_NETWORK_ERR_PRINT_WR_FILE_NOT_EXIST || result == BAMBU_NETWORK_ERR_PRINT_SP_FILE_NOT_EXIST) {
            msg_text = file_is_not_exists_str;
        } else if (result == BAMBU_NETWORK_ERR_PRINT_SP_FILE_OVER_SIZE || result == BAMBU_NETWORK_ERR_PRINT_WR_FILE_OVER_SIZE) {
            msg_text = file_over_size_str;
        } else if (result == BAMBU_NETWORK_ERR_PRINT_WR_CHECK_MD5_FAILED || result == BAMBU_NETWORK_ERR_PRINT_SP_CHECK_MD5_FAILED) {
            msg_text = failed_in_cloud_service_str;
        } else if (result == BAMBU_NETWORK_ERR_PRINT_WR_GET_NOTIFICATION_TIMEOUT || result == BAMBU_NETWORK_ERR_PRINT_SP_GET_NOTIFICATION_TIMEOUT) {
            msg_text = timeout_to_upload_str;
        } else if (result == BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED || result == BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED) {
            msg_text = upload_ftp_failed_str;
        } else if (result == BAMBU_NETWORK_ERR_CANCELED) {
            msg_text = print_canceled_str;
            ctl.update_status(0, msg_text);
        } else {
            msg_text = send_print_failed_str;
        }

        if (result != BAMBU_NETWORK_ERR_CANCELED) {
            ctl.show_error_info(msg_text, 0, "", "");
        }

        BOOST_LOG_TRIVIAL(error) << "print_job: failed, result = " << result;
    } else {
        // wait for printer mqtt ready the same job id

        wxGetApp().plater()->record_slice_preset("print");

        BOOST_LOG_TRIVIAL(error) << "print_job: send ok.";
        wxCommandEvent* evt = new wxCommandEvent(m_print_job_completed_id);
        if (!m_completed_evt_data.empty())
            evt->SetString(m_completed_evt_data);
        else
            evt->SetString(m_dev_id);
        if (m_print_job_completed_id == wxGetApp().plater()->get_send_calibration_finished_event()) {
            int sel = wxGetApp().mainframe->get_calibration_curr_tab();
            if (sel >= 0) {
                evt->SetInt(sel);
            }
        }
        wxQueueEvent(m_plater, evt);
        m_job_finished = true;
    }
}

void PrintJob::finalize(bool canceled, std::exception_ptr &eptr) {
    try {
        if (eptr)
            std::rethrow_exception(eptr);
        eptr = nullptr;
    } catch (...) {
        eptr = std::current_exception();
    }

    if (canceled || eptr)
        return;
}

void PrintJob::set_project_name(std::string name)
{
    m_project_name = name;
}

void PrintJob::set_dst_name(std::string path)
{
    m_dst_path = path;
}


void PrintJob::on_check_ip_address_fail(std::function<void()> func)
{
    m_enter_ip_address_fun_fail = func;
}

void PrintJob::on_check_ip_address_success(std::function<void()> func)
{
    m_enter_ip_address_fun_success = func;
}

// void PrintJob::connect_to_local_mqtt()
// {
//     this->update_status(0, wxEmptyString);
// }

void PrintJob::set_calibration_task(bool is_calibration)
{
    m_is_calibration_task = is_calibration;
}

}} // namespace Slic3r::GUI
