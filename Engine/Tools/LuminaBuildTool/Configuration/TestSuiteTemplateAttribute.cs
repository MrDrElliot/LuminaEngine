namespace LuminaBuildTool.Configuration;

// Names the TargetRules base a synthesized test suite builds from, without which it falls back to bare TargetRules.
[AttributeUsage(AttributeTargets.Class)]
public sealed class TestSuiteTargetTemplateAttribute : Attribute
{
}

// The ModuleRules counterpart, supplying a suite the defaults every module in the tree gets.
[AttributeUsage(AttributeTargets.Class)]
public sealed class TestSuiteModuleTemplateAttribute : Attribute
{
}

// Marks a hand-written target whose executable is a test runner, so LuminaBuild Test builds and runs it with the suites.
[AttributeUsage(AttributeTargets.Class)]
public sealed class TestTargetAttribute : Attribute
{
}
