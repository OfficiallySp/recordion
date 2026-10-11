#pragma once

#include <string>
#include <string_view>

namespace rec {

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view utf16);

} // namespace rec
