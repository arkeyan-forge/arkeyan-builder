// VersionResource: what windres gets for a built game's executable.
#include "version_resource.h"
#include <cstdio>

static int failures = 0;

static void Expect(const std::string& text, const std::string& line)
{
    if (text.find(line + "\n") == std::string::npos)
    {
        printf("FAIL: no line \"%s\"\n", line.c_str());
        failures++;
    }
}

int TestAccentedBuildPaths(int argc, char** argv);

int main(int argc, char** argv)
{
    if (argc > 1)
        return TestAccentedBuildPaths(argc, argv);
    failures += TestAccentedBuildPaths(argc, argv);
    std::string rc = VersionResource("Skylanders Giants", "Skylanders Giants", "0.8.2");
    Expect(rc, "FILEVERSION 0,8,2,0");
    Expect(rc, "PRODUCTVERSION 0,8,2,0");
    Expect(rc, "            VALUE \"CompanyName\", \"Arkeyan project\"");
    Expect(rc, "            VALUE \"LegalCopyright\", \"Copyright (c) 2026 the SSARecomp authors; third-party notices "
               "apply\"");
    Expect(rc, "            VALUE \"FileDescription\", \"Arkeyan runtime: Skylanders Giants\"");
    Expect(rc, "            VALUE \"ProductName\", \"Arkeyan runtime: Skylanders Giants\"");
    Expect(rc, "            VALUE \"FileVersion\", \"0.8.2\"");
    Expect(rc, "            VALUE \"OriginalFilename\", \"Skylanders Giants.exe\"");
    Expect(VersionResource("Skylanders", "Skylanders Spyro's Adventure", "1.2.3.4"), "FILEVERSION 1,2,3,4");
    Expect(VersionResource("Skylanders", "Skylanders Spyro's Adventure", "1.0"), "FILEVERSION 1,0,0,0");
    if (failures == 0)
        printf("ok\n");
    return failures != 0;
}
