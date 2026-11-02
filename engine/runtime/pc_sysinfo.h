// pc_sysinfo.h: the player's name masked out of a UTF-8 text (pc_sysinfo.cpp; F10's "Copy system info")
#pragma once
#include <string>

// text with the profile folder as %USERPROFILE% and the user name (3 characters or more) as <user>, case-insensitive
std::string pc_mask_user(const std::string &text, const std::wstring &profile, const std::wstring &user);
std::string pc_mask_user(const std::string &text);   // this process's USERPROFILE and USERNAME
