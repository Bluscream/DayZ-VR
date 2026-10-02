#include "../common/config_number.hpp"

#include <iostream>
#include <stdexcept>

int main()
{
    using dayz::config_number::ParseFloat;
    for (const auto* text : {L"", L" ", L"nan", L"-nan", L"inf", L"-inf", L"1e999",
        L"1e-999", L"0.5junk", L"0.5 1", L"+", L".x"})
        if (ParseFloat(text, 0.8f) != 0.8f)
            throw std::runtime_error("invalid config float was accepted");
    if (ParseFloat(L" 0.5 \t", 0.8f) != 0.5f || ParseFloat(L"-2.5e1", 0.8f) != -25.0f ||
        ParseFloat(L"0", 0.8f) != 0.0f)
        throw std::runtime_error("valid config float was rejected");
    std::cout << "config_number_test: all checks passed\n";
}
