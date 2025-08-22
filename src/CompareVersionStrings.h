#pragma once
#include <sstream>
#include <string>
#include <vector>

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