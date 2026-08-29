namespace LuminaBuildTool.Configuration;

// Names the TargetRules class a synthesized test suite is built from, since the tool cannot know the
// tree's own base class. Without one a suite falls back to bare TargetRules and no global definitions.
[AttributeUsage(AttributeTargets.Class)]
public sealed class TestSuiteTargetTemplateAttribute : Attribute
{
}

// The ModuleRules counterpart, supplying a suite the defaults every module in the tree gets.
[AttributeUsage(AttributeTargets.Class)]
public sealed class TestSuiteModuleTemplateAttribute : Attribute
{
}
