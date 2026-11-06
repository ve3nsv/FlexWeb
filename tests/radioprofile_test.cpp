// radioProfile (include/radioprofile.h): the --radio profiles of punch list 73.
#include "radioprofile.h"
#include <cstdio>

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("FAIL %s\n", msg); ++failures; } } while (0)

int main()
{
    QString err;
    radioProfile f = radioProfile::parse("name=Flex 6400;type=flex;amp=10.0.0.25", &err);
    CHECK(f.valid() && err.isEmpty(), "flex profile valid");
    CHECK(f.isFlex() && f.host == "auto", "a flex profile without a host finds the radio (auto)");
    CHECK(f.amp == "10.0.0.25", "amp kept");

    radioProfile i = radioProfile::parse("name=IC-7300;type=icomlan;host=100.74.78.87;user=va3nsv;pass=a\\;b\\\\c;radio=7300;buffer=300", &err);
    CHECK(i.valid() && !i.isFlex(), "icomlan profile valid");
    CHECK(i.pass == "a;b\\c", "escaped ; and \\ in a value");
    CHECK(i.radio == "7300" && i.bufferMs == 300 && i.amp.isEmpty(), "radio, buffer, no amp");

    radioProfile r = radioProfile::parse(i.toSpec());
    CHECK(r.name == i.name && r.pass == i.pass && r.host == i.host && r.radio == i.radio && r.bufferMs == 300,
          "toSpec / parse round trip");

    radioProfile bad = radioProfile::parse("name=X;type=icomlan", &err);
    CHECK(!bad.valid() && !err.isEmpty(), "icomlan without a host refused");
    bad = radioProfile::parse("type=flex", &err);
    CHECK(!bad.valid() && err == "no name", "no name refused");
    bad = radioProfile::parse("name=Y;type=yaesu", &err);
    CHECK(!bad.valid() && !err.isEmpty(), "unknown type refused");
    radioProfile u = radioProfile::parse("name=Z;type=smartlink;future=1", &err);
    CHECK(u.valid() && u.host == "auto", "unknown keys ignored, smartlink auto");

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
