// A "new launcher" that can't start: exits 1 for every command, --self-test included.
// The selftest-fail scenario ships it as sc-offline.exe so the update must put the old files back (#31 D5).
int wmain() { return 1; }
