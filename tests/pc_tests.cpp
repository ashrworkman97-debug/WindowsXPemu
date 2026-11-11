#include "xprevival/pc.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
int failures = 0;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
#define CHECK(expression) check((expression), #expression, __LINE__)
}

int main() {
    using xprevival::Pc;

    Pc pc(xprevival::make_demo_disk_image(), 4u * 1024u * 1024u);
    CHECK(!pc.booted());

    bool rejected_run_before_boot = false;
    try { (void)pc.run(1); }
    catch (const std::logic_error&) { rejected_run_before_boot = true; }
    CHECK(rejected_run_before_boot);

    pc.boot();
    CHECK(pc.booted());
    CHECK(pc.cpu().registers().cs == 0);
    CHECK(pc.cpu().registers().eip == 0x7C00);
    CHECK((pc.cpu().registers().edx & 0xFFu) == 0x80u);

    const auto executed = pc.run(128);
    CHECK(executed > 10);
    CHECK(pc.cpu().halted());
    CHECK(pc.cpu().last_error().empty());
    CHECK(pc.cpu().read16(0x0600) == 0x0040); // ATA IDENTIFY word 0
    CHECK(pc.text_screen().find("XP REVIVAL - ATA BOOT OK") != std::string::npos);

    bool rejected_second_boot = false;
    try { pc.boot(); }
    catch (const std::logic_error&) { rejected_second_boot = true; }
    CHECK(rejected_second_boot);

    auto invalid_image = xprevival::make_demo_disk_image();
    invalid_image[510] = 0;
    Pc invalid_pc(std::move(invalid_image), 2u * 1024u * 1024u);
    bool rejected_missing_signature = false;
    try { invalid_pc.boot(); }
    catch (const std::runtime_error&) { rejected_missing_signature = true; }
    CHECK(rejected_missing_signature);

    if (failures != 0) {
        std::cerr << failures << " PC test(s) failed.\n";
        return EXIT_FAILURE;
    }

    std::cout << "PC boot tests passed.\n";
    return EXIT_SUCCESS;
}
