#include "TestFile.hpp"

#include <boost/thread.hpp>

#include <iostream>
#include <cinttypes>
#include <vector>

constexpr uint32_t someInt = 23;

std::vector<uint32_t> someInts = {
    0,
    1,
    9
};

void someFunction()
{
    std::cout << "Yep, here we go - someFunction in cpp called." << std::endl;
}
