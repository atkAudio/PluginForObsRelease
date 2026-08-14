#pragma once

#include <cctype>
#include <sstream>
#include <string>
#include <vector>

inline std::string NormalizeVersionString(const std::string& version)
{
    auto start = version.find_first_of("0123456789");
    if (start == std::string::npos)
        return {};

    std::string normalized;
    bool lastWasDot = false;
    for (size_t i = start; i < version.size(); ++i)
    {
        auto ch = static_cast<unsigned char>(version[i]);
        if (std::isdigit(ch) != 0)
        {
            normalized.push_back(static_cast<char>(ch));
            lastWasDot = false;
            continue;
        }

        if (ch == '.' && !lastWasDot)
        {
            normalized.push_back('.');
            lastWasDot = true;
            continue;
        }

        break;
    }

    while (!normalized.empty() && normalized.back() == '.')
        normalized.pop_back();

    return normalized;
}

inline std::vector<int> TokenizeVersionString(const std::string& str)
{
    std::vector<int> tokens;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, '.'))
    {
        try
        {
            tokens.push_back(std::stoi(item));
        }
        catch (const std::exception& e)
        {
            (void)e;
            tokens.push_back(-1);
        }
    }
    return tokens;
}

inline int CompareVersionStrings(const std::string& v1, const std::string& v2)
{
    auto p1 = TokenizeVersionString(v1);
    auto p2 = TokenizeVersionString(v2);
    size_t maxLen = std::max(p1.size(), p2.size());
    p1.resize(maxLen, 0);
    p2.resize(maxLen, 0);
    for (size_t i = 0; i < maxLen; ++i)
    {
        if (p1[i] < p2[i])
            return -1;
        if (p1[i] > p2[i])
            return 1;
    }
    return 0;
}
