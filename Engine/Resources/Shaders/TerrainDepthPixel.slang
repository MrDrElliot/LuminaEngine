// Stamps empty (0) into the VisBuffer where terrain is closest, so classify and deferred skip the mesh pixels it covers.

[shader("fragment")]
uint FragmentMain() : SV_Target0
{
    return 0u;
}
