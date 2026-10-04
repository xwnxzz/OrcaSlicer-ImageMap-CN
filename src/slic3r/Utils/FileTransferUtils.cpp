#include <wx/wx.h>
#include <type_traits>
#include <stdexcept>
#include <boost/log/trivial.hpp>
#include "FileTransferUtils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"

namespace Slic3r {

FileTransferModule::FileTransferModule(ModuleHandle networking_module, int required_abi_version) : networking_(networking_module)
{
    // basic
    ft_abi_version        = sym_lookup<fn_ft_abi_version>(networking_, "ft_abi_version");
    ft_free               = sym_lookup<fn_ft_free>(networking_, "ft_free");
    ft_job_result_destroy = sym_lookup<fn_ft_job_result_destroy>(networking_, "ft_job_result_destroy");
    ft_job_msg_destroy    = sym_lookup<fn_ft_job_msg_destroy>(networking_, "ft_job_msg_destroy");

    // DSH-PATCH: validate the plug-in ABI before anything is called through it.
    required_abi_ = required_abi_version;
    plugin_abi_   = -1;
    abi_ok_       = false;
    if (ft_abi_version) {
        const int reported = ft_abi_version();
        plugin_abi_ = reported;
        abi_ok_     = (reported == required_abi_version);
        BOOST_LOG_TRIVIAL(info) << "FileTransferModule: plug-in ft_abi_version=" << reported
                                << ", required=" << required_abi_version
                                << ", abi_ok=" << (abi_ok_ ? "true" : "false");
        if (!abi_ok_) {
            BOOST_LOG_TRIVIAL(error) << "FileTransferModule: ft_* ABI mismatch - the networking plug-in "
                                        "does not match this build; file-transfer operations are disabled";
        }
    } else {
        BOOST_LOG_TRIVIAL(error) << "FileTransferModule: plug-in does not export ft_abi_version; "
                                    "the ft_* file-transfer API is unavailable";
    }

    // tunnel
    ft_tunnel_create        = sym_lookup<fn_ft_tunnel_create>(networking_, "ft_tunnel_create");
    ft_tunnel_retain        = sym_lookup<fn_ft_tunnel_retain>(networking_, "ft_tunnel_retain");
    ft_tunnel_release       = sym_lookup<fn_ft_tunnel_release>(networking_, "ft_tunnel_release");
    ft_tunnel_start_connect = sym_lookup<fn_ft_tunnel_start_connect>(networking_, "ft_tunnel_start_connect");
    ft_tunnel_sync_connect  = sym_lookup<fn_ft_tunnel_sync_connect>(networking_, "ft_tunnel_sync_connect");
    ft_tunnel_set_status_cb = sym_lookup<fn_ft_tunnel_set_status_cb>(networking_, "ft_tunnel_set_status_cb");
    ft_tunnel_shutdown      = sym_lookup<fn_ft_tunnel_shutdown>(networking_, "ft_tunnel_shutdown");

    // job
    ft_job_create        = sym_lookup<fn_ft_job_create>(networking_, "ft_job_create");
    ft_job_retain        = sym_lookup<fn_ft_job_retain>(networking_, "ft_job_retain");
    ft_job_release       = sym_lookup<fn_ft_job_release>(networking_, "ft_job_release");
    ft_job_set_result_cb = sym_lookup<fn_ft_job_set_result_cb>(networking_, "ft_job_set_result_cb");
    ft_job_get_result    = sym_lookup<fn_ft_job_get_result>(networking_, "ft_job_get_result");
    ft_tunnel_start_job  = sym_lookup<fn_ft_tunnel_start_job>(networking_, "ft_tunnel_start_job");
    ft_job_cancel        = sym_lookup<fn_ft_job_cancel>(networking_, "ft_job_cancel");

    ft_job_set_msg_cb  = sym_lookup<fn_ft_job_set_msg_cb>(networking_, "ft_job_set_msg_cb");
    ft_job_try_get_msg = sym_lookup<fn_ft_job_try_get_msg>(networking_, "ft_job_try_get_msg");
    ft_job_get_msg     = sym_lookup<fn_ft_job_get_msg>(networking_, "ft_job_get_msg");
}

FileTransferTunnel::FileTransferTunnel(FileTransferModule &m, const std::string &url) : m_(&m)
{
    // DSH-PATCH: refuse to touch the plug-in when its ft_* ABI is not the one this
    // build was compiled against - calling through it crashed inside the plug-in.
    if (!m_->abi_ok()) {
        BOOST_LOG_TRIVIAL(error) << "FileTransferTunnel: refusing to create a tunnel, plug-in ft_* ABI mismatch "
                                    "(plugin=" << m_->plugin_abi() << ", required match failed)";
        h_ = nullptr;
        return;
    }
    FT_TunnelHandle *h{};
    // DSH-PATCH: the plug-in API may be absent or may refuse to create the tunnel.
    // Never leave a null handle behind: every later call is handed straight to the
    // closed-source plug-in and dereferences it (ACCESS_VIOLATION).
    if (!m_->ft_tunnel_create) {
        BOOST_LOG_TRIVIAL(error) << "FileTransferTunnel: ft_tunnel_create is not exported by the networking plug-in";
        h_ = nullptr;
        return;
    }
    const ft_err create_err = m_->ft_tunnel_create(url.c_str(), &h);
    if (create_err != FT_OK || !h) {
        BOOST_LOG_TRIVIAL(error) << "FileTransferTunnel: ft_tunnel_create failed, err=" << (int) create_err
                                 << ", handle=" << (h ? "set" : "null") << ", url=" << url;
        h_ = nullptr;
        return;
    }
    h_ = h;

    // C API: ft_status_cb(void* user, int old_status, int new_status, int err, const char* msg)
    auto tramp = [](void *user, int old_status, int new_status, int err_code, const char *msg) noexcept {
        auto *self    = reinterpret_cast<FileTransferTunnel *>(user);
        if (!self) return;
        self->status_ = new_status;
        if (!self->status_cb_) return;
        try {
            self->status_cb_(old_status, new_status, err_code, std::string(msg ? msg : ""));
        } catch (...) {}
    };
    if (!m_->ft_tunnel_set_status_cb) {
        BOOST_LOG_TRIVIAL(error) << "FileTransferTunnel: ft_tunnel_set_status_cb is not exported by the networking plug-in";
        return;
    }
    if (m_->ft_tunnel_set_status_cb(h_, tramp, this) == ft_err::FT_EXCEPTION) { throw std::runtime_error("ft_tunnel_set_status_cb failed"); }
}

void FileTransferTunnel::start_connect()
{
    // DSH-PATCH: refuse to call into the plug-in without a valid tunnel handle.
    if (!h_ || !m_->ft_tunnel_start_connect) {
        BOOST_LOG_TRIVIAL(error) << "FileTransferTunnel::start_connect: no usable tunnel handle/plug-in entry point";
        throw std::runtime_error("ft_tunnel_start_connect unavailable");
    }
    // C API: ft_conn_cb(void* user, int ok, int err, const char* msg)
    auto tramp = [](void *user, int ok, int ec, const char *msg) noexcept {
        auto *pcb = reinterpret_cast<ConnectionCb *>(user);
        if (!pcb) return;
        try {
            (*pcb)(ok == 0, ec, std::string(msg ? msg : ""));
        } catch (...) {}
    };
    if (m_->ft_tunnel_start_connect(h_, tramp, &conn_cb_) == ft_err::FT_EXCEPTION) { throw std::runtime_error("ft_tunnel_start_connect failed"); }
}

bool FileTransferTunnel::sync_start_connect()
{
    // DSH-PATCH: this is the exact call that crashed the application with an
    // ACCESS_VIOLATION. A null handle was passed to the closed-source plug-in.
    // Report failure instead of crashing; callers already fall back to FTP-only.
    if (!h_ || !m_->abi_ok() || !m_->ft_tunnel_sync_connect) {
        BOOST_LOG_TRIVIAL(error) << "FileTransferTunnel::sync_start_connect: no usable tunnel handle/plug-in entry point";
        return false;
    }
    return m_->ft_tunnel_sync_connect(h_) == FT_OK;
}

void FileTransferTunnel::on_connection(ConnectionCb cb) { conn_cb_ = std::move(cb); }
void FileTransferTunnel::on_status(TunnelStatusCb cb) { status_cb_ = std::move(cb); }

void FileTransferTunnel::shutdown()
{
    if (h_ && m_->ft_tunnel_shutdown) (void) m_->ft_tunnel_shutdown(h_);
}

FileTransferJob::FileTransferJob(FileTransferModule &m, const std::string &params_json) : m_(&m)
{
    // DSH-PATCH: same ABI gate as FileTransferTunnel.
    if (!m_->abi_ok()) {
        BOOST_LOG_TRIVIAL(error) << "FileTransferJob: refusing to create a job, plug-in ft_* ABI mismatch";
        h_ = nullptr;
        return;
    }
    FT_JobHandle *h{};
    // DSH-PATCH: same defect as FileTransferTunnel - do not keep a null handle
    // that would later be passed to the closed-source plug-in.
    if (!m_->ft_job_create) {
        BOOST_LOG_TRIVIAL(error) << "FileTransferJob: ft_job_create is not exported by the networking plug-in";
        h_ = nullptr;
        return;
    }
    const ft_err create_err = m_->ft_job_create(params_json.c_str(), &h);
    if (create_err != FT_OK || !h) {
        BOOST_LOG_TRIVIAL(error) << "FileTransferJob: ft_job_create failed, err=" << (int) create_err
                                 << ", handle=" << (h ? "set" : "null");
        h_ = nullptr;
        return;
    }
    h_ = h;

    // C API: ft_job_result_cb(void* user, int tunnel_err, ft_job_result result)
    auto tramp = [](void *user, ft_job_result r) noexcept {
        auto *self = reinterpret_cast<FileTransferJob *>(user);
        if (!self) return;

        try {
            self->finished_ = true;
            self->solve_result(r);

            if (self->result_cb_) self->result_cb_(self->res_, self->resp_ec_, self->res_json_, self->res_bin_);
            // DSH-PATCH: only call the plug-in when it actually exports the entry point.
            if (self->m_ && self->m_->ft_job_result_destroy) self->m_->ft_job_result_destroy(&r);
        } catch (...) {
            // swallow
        }

        try {
            if (auto *mod = self ? self->m_ : nullptr) {
                if (mod->ft_job_result_destroy)
                    mod->ft_job_result_destroy(&r);
                else if (mod->ft_free) {
                    if (r.json) mod->ft_free((void *) r.json);
                    if (r.bin) mod->ft_free((void *) r.bin);
                }
            }
        } catch (...) {}
    };

    if (!m_->ft_job_set_result_cb) {
        BOOST_LOG_TRIVIAL(error) << "FileTransferJob: ft_job_set_result_cb is not exported by the networking plug-in";
        return;
    }
    if (m_->ft_job_set_result_cb(h_, tramp, this) == ft_err::FT_EXCEPTION) { throw std::runtime_error("ft_job_set_result_cb failed"); }
}

void FileTransferJob::on_result(ResultCb cb) { result_cb_ = std::move(cb); }

bool FileTransferJob::get_result(int &ec, int &resp_ec, std::string &json, std::vector<std::byte> &bin, uint32_t timeout_ms)
{
    if (!h_) throw std::runtime_error("job handle invalid");
    // DSH-PATCH: guard the plug-in entry point.
    if (!m_->ft_job_get_result) throw std::runtime_error("ft_job_get_result unavailable");
    ft_job_result result;
    if (m_->ft_job_get_result(h_, timeout_ms, &result) == ft_err::FT_EXCEPTION) return false;
    solve_result(result);
    m_->ft_job_result_destroy(&result);
    ec = res_;
    resp_ec = res_;
    json    = res_json_;
    bin     = res_bin_;
    return true;
}

void FileTransferJob::start_on(FileTransferTunnel &t)
{
    if (!h_) throw std::runtime_error("job handle invalid");
    // DSH-PATCH: guard the plug-in entry point.
    if (!m_->ft_tunnel_start_job) throw std::runtime_error("ft_tunnel_start_job unavailable");
    if (m_->ft_tunnel_start_job(t.native(), h_) == ft_err::FT_EXCEPTION) { throw std::runtime_error("ft_tunnel_start_job failed"); }
}

void FileTransferJob::on_msg(MsgCb cb)
{
    msg_cb_ = std::move(cb);
    if (!h_) return;

    // C API: ft_job_msg_cb(void* user, ft_job_msg msg)
    auto tramp = [](void *user, ft_job_msg m) noexcept {
        auto *self = reinterpret_cast<FileTransferJob *>(user);
        if (!self) return;
        try {
            if (self->msg_cb_) { self->msg_cb_(m.kind, std::string(m.json ? m.json : "")); }
        } catch (...) {}

        try {
            if (auto *mod = self->m_) {
                if (mod->ft_job_msg_destroy)
                    mod->ft_job_msg_destroy(&m);
                else if (mod->ft_free && m.json)
                    mod->ft_free((void *) m.json);
            }
        } catch (...) {}
    };

    if (!m_->ft_job_set_msg_cb) {
        BOOST_LOG_TRIVIAL(error) << "FileTransferJob::on_msg: ft_job_set_msg_cb is not exported by the networking plug-in";
        return;
    }
    if (m_->ft_job_set_msg_cb(h_, tramp, this) == ft_err::FT_EXCEPTION) { throw std::runtime_error("ft_job_set_msg_cb failed"); }
}

bool FileTransferJob::try_get_msg(int &kind, std::string &json)
{
    if (!h_ || !m_->ft_job_try_get_msg) return false;
    ft_job_msg m{};
    int        rc = m_->ft_job_try_get_msg(h_, &m);
    if (rc != 0) return false;

    kind = m.kind;
    json.assign(m.json ? m.json : "");

    // DSH-PATCH: explicit nullptr tests instead of implicit pointer-to-bool conversion.
    if (m_->ft_job_msg_destroy != nullptr)
        m_->ft_job_msg_destroy(&m);
    else if (m_->ft_free != nullptr && m.json != nullptr)
        m_->ft_free((void *) m.json);

    return true;
}

bool FileTransferJob::get_msg(uint32_t timeout_ms, int &kind, std::string &json){
    if (!h_ || !m_->ft_job_get_msg) return false;
    ft_job_msg m{};
    int        rc = m_->ft_job_get_msg(h_, timeout_ms, &m);
    if (rc != 0) return false;

    kind = m.kind;
    json.assign(m.json ? m.json : "");

    // DSH-PATCH: explicit nullptr tests instead of implicit pointer-to-bool conversion.
    if (m_->ft_job_msg_destroy != nullptr)
        m_->ft_job_msg_destroy(&m);
    else if (m_->ft_free != nullptr && m.json != nullptr)
        m_->ft_free((void *) m.json);

    return true;
}

void FileTransferJob::solve_result(ft_job_result result)
{
    res_     = result.ec;
    resp_ec_ = result.resp_ec;

    res_bin_.clear();
    if (result.bin && result.bin_size) res_bin_.assign(reinterpret_cast<const std::byte *>(result.bin),
        reinterpret_cast<const std::byte *>(result.bin) + result.bin_size);
    res_json_.assign(result.json ? result.json : "");
}

} // namespace Slic3r