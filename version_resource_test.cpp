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

int main()
{
    std::string rc = VersionResource("Skylanders: Giants", "SGRecomp", "0.8.2");
    Expect(rc, "FILEVERSION 0,8,2,0");
    Expect(rc, "PRODUCTVERSION 0,8,2,0");
    Expect(rc, "            VALUE \"FileDescription\", \"Skylanders: Giants\"");
    Expect(rc, "            VALUE \"FileVersion\", \"0.8.2\"");
    Expect(rc, "            VALUE \"OriginalFilename\", \"SGRecomp.exe\"");
    Expect(VersionResource("Skylanders", "SSARecomp", "1.2.3.4"), "FILEVERSION 1,2,3,4");
    Expect(VersionResource("Skylanders", "SSARecomp", "1.0"), "FILEVERSION 1,0,0,0");
    if (failures == 0)
        printf("ok\n");
    return failures != 0;
}
