#include "TestFile.hpp"

#include <boost/thread.hpp>

#include <iostream>
#include <cinttypes>

constexpr uint32_t someInt = 23;

void someFunction()
{
    std::cout << "Yep, here we go - someFunction in cpp called." << std::endl;
}
