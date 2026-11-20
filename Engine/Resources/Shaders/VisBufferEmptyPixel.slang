// Stamps empty (0) into the VisBuffer where custom depth geometry is closest, so classify and deferred skip the pixels it covers.

[shader("fragment")]
uint FragmentMain() : SV_Target0
{
    return 0u;
}
