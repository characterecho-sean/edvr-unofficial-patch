// Deliberately has no VERSIONINFO resource. The build rig compiles this file
// as a DLL and passes its absolute path only to the test executable's
// --self-test mode.
extern "C" __declspec(dllexport) unsigned long long
edvrNoVersionFixture(unsigned long long value) {
    return value ^ 0xEDBULL;
}
