#include "Storage.h"
#include "Util.h"

bool RackHostDataLoad(const SnowAirHost* host, const wchar_t* key, std::string& out)
{
    out.clear();
    if (!host || host->api_version < 5 || !host->local_data_get || !key) return false;
    const int need = host->local_data_get(host->user, key, nullptr, 0);
    if (need <= 1) return false;
    std::wstring text(static_cast<size_t>(need), L'\0');
    const int got = host->local_data_get(host->user, key, text.data(), need);
    if (got <= 1) return false;
    if (!text.empty() && text.back() == L'\0') text.pop_back();
    out = util::WideToUtf8(text);
    return !out.empty();
}

bool RackHostDataSave(const SnowAirHost* host, const wchar_t* key, const std::string& utf8)
{
    if (!host || host->api_version < 5 || !host->local_data_set || !key || utf8.empty()) return false;
    const std::wstring text = util::Utf8ToWide(utf8);
    return host->local_data_set(host->user, key, text.c_str()) != 0;
}
