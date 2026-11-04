#include <string.h>
#include "unity.h"
#include "vendor_info.h"

TEST_CASE("Stamped version: date and 7 hex hash", "[vendor_info]")
{
    char out[17];
    br_core_vendor_sw_version("20261104T095955Z-bb169ca", out);
    TEST_ASSERT_EQUAL_STRING("20261104-bb169ca", out);
}

TEST_CASE("Dirty stamped version: -dirty is dropped", "[vendor_info]")
{
    char out[17];
    br_core_vendor_sw_version("20261104T095955Z-bb169ca-dirty", out);
    TEST_ASSERT_EQUAL_STRING("20261104-bb169ca", out);
}

TEST_CASE("Short version: copied whole", "[vendor_info]")
{
    char out[17];
    br_core_vendor_sw_version("abc", out);
    TEST_ASSERT_EQUAL_STRING("abc", out);
}

TEST_CASE("Empty version: empty string", "[vendor_info]")
{
    char out[17];
    memset(out, 'x', sizeof(out));
    br_core_vendor_sw_version("", out);
    TEST_ASSERT_EQUAL_STRING("", out);
}

TEST_CASE("Malformed long version: first 16 bytes, NUL-terminated, nothing past out[16]", "[vendor_info]")
{
    char out[18];
    memset(out, 'x', sizeof(out));
    br_core_vendor_sw_version("v1.2.3-this-is-a-very-long-version", out);
    TEST_ASSERT_EQUAL_STRING("v1.2.3-this-is-a", out);
    TEST_ASSERT_EQUAL('x', out[17]);
}

TEST_CASE("Version with a wrong hash length: first 16 bytes", "[vendor_info]")
{
    char out[17];
    br_core_vendor_sw_version("20261104T095955Z-bb169", out);
    TEST_ASSERT_EQUAL_STRING("20261104T095955Z", out);
}

TEST_CASE("Version with a non-hex hash: first 16 bytes", "[vendor_info]")
{
    char out[17];
    br_core_vendor_sw_version("20261104T095955Z-zzzzzzz", out);
    TEST_ASSERT_EQUAL_STRING("20261104T095955Z", out);
}

TEST_CASE("Null version: empty string", "[vendor_info]")
{
    char out[17];
    memset(out, 'x', sizeof(out));
    br_core_vendor_sw_version(NULL, out);
    TEST_ASSERT_EQUAL_STRING("", out);
}
