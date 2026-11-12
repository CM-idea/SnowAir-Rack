#pragma once

#include "Common.h"
#include <string>

/* 晾衣架数据统一走宿主：新宿主写 SnowAir 的 local.json，旧宿主返回 false 走本地文件。 */
bool RackHostDataLoad(const SnowAirHost* host, const wchar_t* key, std::string& out);
bool RackHostDataSave(const SnowAirHost* host, const wchar_t* key, const std::string& utf8);
