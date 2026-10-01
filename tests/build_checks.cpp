#include "dayz_build_checks.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 2)
        throw std::runtime_error("Pass the mapped 1.29.163709 executable image.");
    std::ifstream input(argv[1], std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot open the executable image.");
    std::vector<std::uint8_t> image(
        std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    if (!dayz::builds::ValidateCurrentBuild(image))
        throw std::runtime_error("The real executable failed the instruction checks.");
    std::size_t mutations{};
    for (const auto& check : dayz::builds::kCurrentBuildChecks)
    {
        for (std::size_t offset = 0; offset < check.bytes.size(); ++offset)
        {
            image[check.rva + offset] ^= 1;
            if (dayz::builds::ValidateCurrentBuild(image))
                throw std::runtime_error("A modified instruction was accepted.");
            image[check.rva + offset] ^= 1;
            ++mutations;
        }
        if (dayz::builds::ValidateCurrentBuild(
                std::span<const std::uint8_t>{image}.first(check.rva)))
            throw std::runtime_error("A truncated executable was accepted.");
    }
    if (dayz::builds::ValidateCurrentBuild({}))
        throw std::runtime_error("An empty executable was accepted.");
    std::cout << "Real executable accepted; " << mutations
              << " instruction mutations, 13 truncations, and empty input rejected.\n";
}
