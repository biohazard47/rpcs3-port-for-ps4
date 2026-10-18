#pragma once

// PKG Store: the console's fetcher for util/ps4_pkgstore.hpp. libcurl with the port's own DNS (utils::ps4::net::resolve_ipv4
// pinned with CURLOPT_RESOLVE, as https_get in ps4_disc_key.hpp does), http AND https (the PS3 CDN links of a pkgi list are
// plain http), resume with a Range, and redirects followed here, one hop at a time, so every host gets the same pinned DNS.
// The body goes to the part file only for a 200 or 206 answer: an error page never lands in a package.

#ifdef RPCS3_PS4

#include "Utilities/File.h"
#include "util/ps4_net.hpp"
#include "util/ps4_pkgstore.hpp"
#include "util/sysinfo.hpp"
#include "curl/curl/include/curl/curl.h"

#include <array>
#include <cstring>

namespace utils::ps4::pkgstore
{
inline constexpr char ca_bundle[] = "/app0/certs/cacert.pem";

namespace http_detail
{
struct transfer
{
    std::FILE* file = nullptr;
    const fetch_request* request = nullptr;
    std::uint64_t base = 0, written = 0;
    long status = 0;
    std::uint64_t length = 0, range_total = 0; // Content-Length of this answer; the total after the '/' of Content-Range
    std::string location;
    bool cancelled = false, disk_error = false, range_ignored = false;
};

inline bool header_is(std::string_view line, std::string_view name)
{
    return line.size() > name.size() && detail::starts_with_nocase(line, name) && line[name.size()] == ':';
}

inline std::size_t on_header(char* data, std::size_t size, std::size_t count, void* user)
{
    auto& t = *static_cast<transfer*>(user);
    std::string_view line(data, size * count);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    if (detail::starts_with_nocase(line, "HTTP/"))   // a new answer (after a redirect, or the final one): start over
    {
        t.status = 0;
        t.length = t.range_total = 0;
        t.location.clear();
        const std::size_t space = line.find(' ');
        if (space != std::string_view::npos)
        {
            const std::string_view code = detail::trim(line.substr(space + 1, 3));
            long value = 0;
            if (std::from_chars(code.data(), code.data() + code.size(), value).ec == std::errc{}) t.status = value;
        }
    }
    else if (header_is(line, "content-length"))
    {
        const std::string_view v = detail::trim(line.substr(15));
        std::from_chars(v.data(), v.data() + v.size(), t.length);
    }
    else if (header_is(line, "content-range"))       // "bytes 100-999/1000"
    {
        if (const std::size_t slash = line.rfind('/'); slash != std::string_view::npos)
        {
            const std::string_view v = detail::trim(line.substr(slash + 1));
            std::from_chars(v.data(), v.data() + v.size(), t.range_total);
        }
    }
    else if (header_is(line, "location"))
    {
        t.location = std::string(detail::trim(line.substr(9)));
    }
    return size * count;
}

inline std::size_t on_data(char* data, std::size_t size, std::size_t count, void* user)
{
    auto& t = *static_cast<transfer*>(user);
    const std::size_t bytes = size * count;
    if (t.status != 200 && t.status != 206) return bytes;      // an error page or a redirect body: dropped
    if (t.status == 200 && t.base) { t.range_ignored = true; return 0; } // asked to resume, the whole file came: stop
    if (t.request->cancelled && t.request->cancelled()) { t.cancelled = true; return 0; }
    if (std::fwrite(data, 1, bytes, t.file) != bytes) { t.disk_error = true; return 0; }
    t.written += bytes;
    return bytes;
}

inline int on_progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    auto& t = *static_cast<transfer*>(user);
    if (t.request->cancelled && t.request->cancelled()) { t.cancelled = true; return 1; }
    if (t.request->progress && (t.status == 200 || t.status == 206))
    {
        const std::uint64_t total = t.range_total ? t.range_total : (t.length ? t.base + t.length : 0);
        t.request->progress(t.base + t.written, total);
    }
    return 0;
}

inline bool redirect_status(long s) { return s == 301 || s == 302 || s == 303 || s == 307 || s == 308; }
} // namespace http_detail

inline fetch_result http_fetch(const fetch_request& request)
{
    using code = fetch_result::code;
    fetch_result out;
    std::string url = request.url;
    std::string certificates;                           // loaded when the first https URL needs it
    const std::string agent = utils::get_user_agent();

    for (int hop = 0; hop < 5; ++hop)
    {
        const url_parts u = parse_url(url);
        if (!u.ok) { out.detail = "not an http(s) URL"; return out; }
        const int link = utils::ps4::net::link_state();
        if (link >= 0 && link != utils::ps4::net::netctl_state_ip_obtained)
        {
            out.status = code::offline;
            out.detail = u.host + ": network link state " + std::to_string(link) + " (no IP address)";
            return out;
        }
        if (u.secure && certificates.empty())
        {
            const fs::file file(ca_bundle);
            const std::uint64_t size = file ? file.size() : 0;
            if (size && size <= (4u << 20))
            {
                certificates.resize(static_cast<std::size_t>(size));
                if (file.read(certificates.data(), size) != size) certificates.clear();
            }
            if (certificates.find("-----BEGIN CERTIFICATE-----") == std::string::npos)
            {
                out.detail = std::string("no CA bundle at ") + ca_bundle;
                return out;
            }
        }
        std::string how;
        const std::uint32_t address = utils::ps4::net::resolve_ipv4(u.host, how);
        if (!address)
        {
            out.status = code::offline;
            out.detail = u.host + ": DNS failed (" + how + ")";
            return out;
        }
        std::array<std::uint8_t, 4> ip{};
        std::memcpy(ip.data(), &address, sizeof(address));
        const std::string resolve_line = u.host + ":" + std::to_string(u.port) + ":" + std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." +
            std::to_string(ip[2]) + "." + std::to_string(ip[3]);

        http_detail::transfer t;
        CURLcode result = CURLE_OK;
        long http = 0;
        for (int attempt = 0; attempt < 2; ++attempt)  // the second one starts from zero when the server cannot resume
        {
            t = http_detail::transfer{};
            t.request = &request;
            t.base = attempt ? 0 : request.resume_from;
            t.file = std::fopen(request.part_path.c_str(), t.base ? "ab" : "wb");
            if (!t.file) { out.detail = "cannot open " + request.part_path; out.disk_error = true; return out; }

            CURL* curl = curl_easy_init();
            if (!curl) { std::fclose(t.file); out.detail = "curl_easy_init failed"; return out; }
            curl_slist* resolve = curl_slist_append(nullptr, resolve_line.c_str());
            std::array<char, CURL_ERROR_SIZE> error{};
            curl_blob blob{certificates.data(), certificates.size(), CURL_BLOB_COPY};
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);
            curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);        // followed above, host by host
            if (u.secure)
            {
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
                curl_easy_setopt(curl, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
                curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
            }
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);    // no total timeout: packages are large
            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
            curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 262144L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, agent.c_str());
            curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error.data());
            if (t.base) curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(t.base));
            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &http_detail::on_header);
            curl_easy_setopt(curl, CURLOPT_HEADERDATA, &t);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &http_detail::on_data);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &http_detail::on_progress);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
            result = curl_easy_perform(curl);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
            curl_slist_free_all(resolve);
            curl_easy_cleanup(curl);
            const bool flushed = std::fflush(t.file) == 0;
            std::fclose(t.file);
            t.file = nullptr;
            if (!flushed) t.disk_error = true;
            out.detail = resolve_line + " via " + how + ", HTTP " + std::to_string(http) + ", " + std::to_string(t.written) + " bytes";
            if (!t.range_ignored && result != CURLE_RANGE_ERROR) break;
            if (attempt == 0 && t.base) continue;
            break;
        }
        if (t.cancelled || result == CURLE_ABORTED_BY_CALLBACK) { out.status = code::cancelled; return out; }
        if (t.disk_error) { out.disk_error = true; out.detail += "; writing the part file failed"; return out; }
        if (result == CURLE_OK && http_detail::redirect_status(t.status) && !t.location.empty())
        {
            if (parse_url(t.location).ok) { url = t.location; continue; }
            out.detail += "; redirect to an unsupported address";
            return out;
        }
        if (result != CURLE_OK)
        {
            out.detail += std::string("; curl ") + std::to_string(static_cast<int>(result)) + " [" + curl_easy_strerror(result) + "]";
            const bool no_link = result == CURLE_COULDNT_RESOLVE_HOST || result == CURLE_COULDNT_CONNECT || (result == CURLE_OPERATION_TIMEDOUT && !http && !t.written);
            out.status = no_link ? code::offline : code::failed;
            return out;
        }
        if (t.status != 200 && t.status != 206)
        {
            out.status = code::http_error;
            out.http = t.status;
            return out;
        }
        out.status = code::ok;
        out.http = t.status;
        out.expected_total = t.range_total ? t.range_total : (t.length ? (t.status == 206 ? t.base + t.length : t.length) : 0);
        return out;
    }
    out.detail = "too many redirects";
    return out;
}
} // namespace utils::ps4::pkgstore

#endif // RPCS3_PS4
