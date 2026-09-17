#pragma once

#include <string>

namespace WebSettings {

std::string BuildPage(const std::string &authSuffix);
std::string ApplyQuery(const std::string &query);

} // namespace WebSettings
