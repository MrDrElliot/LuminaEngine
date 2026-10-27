#include "RmlKnowledge.h"

namespace Lumina::RmlKnowledge
{
    namespace RmlKnowledgeData
    {
        using namespace EValue;

        constexpr uint32 LengthPercent = Length | Percent;

        constexpr FRmlElement ElementTable[] =
        {
            { "rml", "Root of an RML document, holding <head> and <body>." },
            { "head", "Document header with <title>, <link>, <style> and <script>." },
            { "body", "The visible document. Put data-model here to bind a model to everything inside.", "template" },
            { "title", "Document title, shown by hosts that display one." },
            { "link", "Loads a stylesheet (type=\"text/rcss\") or registers a template (type=\"text/template\").", "rel type href", true },
            { "style", "Inline RCSS that applies to this document." },
            { "script", "Inline or linked script source.", "src" },
            { "template", "Reusable markup. name= registers it and content= names the element that receives the user's children.", "name content src" },
            { "div", "Generic block container." },
            { "span", "Generic inline container." },
            { "p", "Paragraph of text." },
            { "h1", "Top level heading." },
            { "h2", "Second level heading." },
            { "h3", "Third level heading." },
            { "h4", "Fourth level heading." },
            { "h5", "Fifth level heading." },
            { "h6", "Sixth level heading." },
            { "br", "Line break.", "", true },
            { "hr", "Horizontal rule.", "", true },
            { "b", "Bold inline text, styled by RCSS." },
            { "i", "Italic inline text, styled by RCSS." },
            { "em", "Emphasized inline text." },
            { "strong", "Strongly emphasized inline text." },
            { "small", "Small inline text." },
            { "a", "Inline element, usually given an onclick handler." },
            { "img", "Draws an image, a sprite or a UI material.", "src rect sprite width height", true },
            { "button", "Clickable element. RmlUi gives it no behavior beyond events, so pair it with onclick or data-event-click.", "name value disabled" },
            { "input", "Form control whose type= picks text, password, checkbox, radio, range, submit or button.", "type name value checked disabled maxlength size min max step orientation placeholder", true },
            { "textarea", "Multi-line text input.", "name value cols rows wrap maxlength placeholder disabled" },
            { "select", "Drop-down list of <option> children.", "name value disabled" },
            { "option", "One choice of a <select>.", "value selected disabled" },
            { "label", "Text that activates the control named by for=, or the control inside it.", "for" },
            { "form", "Groups controls so a submit control sends their values in an onsubmit event." },
            { "progress", "Bar filled from value= out of max=, styled through its fill-image or its fill child.", "value max direction start-edge" },
            { "progressbar", "Older name for <progress>.", "value max direction start-edge" },
            { "tabset", "Tabbed container. Each <tab> pairs with the <panel> at the same position." },
            { "tab", "A tab title inside a <tabset>." },
            { "panel", "A tab's content inside a <tabset>." },
            { "tabs", "Wrapper RmlUi generates around the tabs of a <tabset>.", "", false, true },
            { "panels", "Wrapper RmlUi generates around the panels of a <tabset>.", "", false, true },
            { "handle", "Drag handle that moves or resizes a target element.", "move_target size_target edge_margin" },
            { "ul", "List container." },
            { "ol", "Ordered list container." },
            { "li", "List item." },
            { "table", "Table container, laid out with display: table." },
            { "thead", "Table header row group." },
            { "tbody", "Table body row group." },
            { "tfoot", "Table footer row group." },
            { "tr", "Table row." },
            { "td", "Table cell." },
            { "th", "Table header cell." },
            { "col", "Table column.", "", true },
            { "colgroup", "Table column group." },
            { "scrollbarvertical", "Vertical scrollbar RmlUi adds to an overflowing element.", "", false, true },
            { "scrollbarhorizontal", "Horizontal scrollbar RmlUi adds to an overflowing element.", "", false, true },
            { "scrollbarcorner", "Square where both scrollbars meet.", "", false, true },
            { "slidertrack", "Track of a scrollbar or range input.", "", false, true },
            { "sliderbar", "Draggable thumb of a scrollbar or range input.", "", false, true },
            { "sliderarrowdec", "Decrement arrow of a scrollbar or range input.", "", false, true },
            { "sliderarrowinc", "Increment arrow of a scrollbar or range input.", "", false, true },
            { "sliderprogress", "Filled part of a range input's track.", "", false, true },
            { "selectbox", "Drop-down list a <select> opens.", "", false, true },
            { "selectvalue", "The chosen value shown in a closed <select>.", "", false, true },
            { "selectarrow", "Arrow button of a <select>.", "", false, true },
            { "fill", "Fill element RmlUi adds inside a <progress>.", "", false, true },
        };

        constexpr FRmlAttribute AttributeTable[] =
        {
            { "id", "", "Unique name for #id selectors, getElementById and template slots." },
            { "class", "", "Classes separated by spaces, each matched by a .class selector." },
            { "style", "", "Inline RCSS declarations that apply to this element only." },
            { "autofocus", "", "Focuses this element when its document is shown.", "", true },
            { "rel", "link", "Relationship of the linked file.", "stylesheet" },
            { "type", "link", "What the linked file holds.", "text/rcss text/template" },
            { "href", "link", "Path of the linked file, relative to this document." },
            { "template", "body", "Name of a template whose content element receives this body's children." },
            { "name", "template", "Name other documents use to apply this template." },
            { "content", "template", "Id of the element inside the template that receives the user's children." },
            { "src", "template", "Name of a registered template to place here." },
            { "src", "script", "Path of the script file." },
            { "src", "img", "Image path, UI material asset path, or material: URI." },
            { "rect", "img", "Region of the source to draw, as x y width height in pixels." },
            { "sprite", "img", "Sprite name from an @spritesheet, used instead of src." },
            { "width", "img", "Width override in pixels." },
            { "height", "img", "Height override in pixels." },
            { "type", "input", "Kind of control.", "text password checkbox radio range submit button" },
            { "name", "", "Name a form reports this control's value under." },
            { "value", "", "Current value of the control." },
            { "checked", "input", "Starts a checkbox or radio button checked.", "", true },
            { "disabled", "", "Disables the control, which also matches :disabled.", "", true },
            { "maxlength", "", "Most characters a text field accepts." },
            { "size", "input", "Width of a text field in characters." },
            { "min", "input", "Lowest value of a range input." },
            { "max", "input", "Highest value of a range input." },
            { "step", "input", "Increment of a range input." },
            { "orientation", "input", "Direction a range input slides.", "horizontal vertical" },
            { "placeholder", "", "Hint text shown while the field is empty, which also matches :placeholder-shown." },
            { "cols", "textarea", "Width in characters." },
            { "rows", "textarea", "Height in lines." },
            { "wrap", "textarea", "How long lines wrap.", "soft hard off" },
            { "selected", "option", "Starts this option selected.", "", true },
            { "for", "label", "Id of the control this label activates." },
            { "max", "progress", "Value at which the bar is full. Defaults to 1." },
            { "value", "progress", "Current fill, from 0 to max." },
            { "direction", "progress", "Direction the bar fills.", "left right top bottom clockwise counter-clockwise" },
            { "start-edge", "progress", "Edge a circular bar starts filling from.", "top right bottom left" },
            { "move_target", "handle", "Element the handle drags, an id or #self, #parent or #document.", "#self #parent #document" },
            { "size_target", "handle", "Element the handle resizes, an id or #self, #parent or #document.", "#self #parent #document" },
            { "edge_margin", "handle", "How far the target may go past its container's edge, a length or percentage." },
            { "name", "button", "Name a form reports this button under." },
        };

        constexpr FRmlNamed EventTable[] =
        {
            { "onclick", "Fires when the element is clicked." },
            { "ondblclick", "Fires on a double click." },
            { "onmousedown", "Fires when a mouse button goes down over the element." },
            { "onmouseup", "Fires when a mouse button is released over the element." },
            { "onmouseover", "Fires when the mouse enters the element." },
            { "onmouseout", "Fires when the mouse leaves the element." },
            { "onmousemove", "Fires as the mouse moves over the element." },
            { "onmousescroll", "Fires when the wheel turns over the element." },
            { "onkeydown", "Fires when a key goes down while the element has focus." },
            { "onkeyup", "Fires when a key is released while the element has focus." },
            { "ontextinput", "Fires when text is typed into the element." },
            { "onfocus", "Fires when the element gains focus." },
            { "onblur", "Fires when the element loses focus." },
            { "onchange", "Fires when a control's value changes." },
            { "onsubmit", "Fires when a form is submitted." },
            { "onload", "Fires when the document has loaded." },
            { "onunload", "Fires when the document unloads." },
            { "onshow", "Fires when the document is shown." },
            { "onhide", "Fires when the document is hidden." },
            { "onresize", "Fires when the element changes size." },
            { "onscroll", "Fires when the element scrolls." },
            { "ondragstart", "Fires when a drag starts on this element." },
            { "ondrag", "Fires as this element is dragged." },
            { "ondragend", "Fires when a drag of this element ends." },
            { "ondragover", "Fires when a dragged element moves over this one." },
            { "ondragout", "Fires when a dragged element leaves this one." },
            { "ondragdrop", "Fires when a dragged element is dropped on this one." },
            { "onanimationend", "Fires when an animation on this element finishes." },
            { "ontransitionend", "Fires when a transition on this element finishes." },
            { "ontabchange", "Fires when the active tab of a tabset changes." },
            { "onhandledrag", "Fires as a handle drags its target." },
        };

        constexpr FRmlProperty PropertyTable[] =
        {
            { "display", "none block inline inline-block flow-root flex inline-flex table inline-table table-row table-row-group table-column table-column-group table-cell", 0, "<keyword>", "How the element generates boxes. flex lays children out along a row or column." },
            { "position", "static relative absolute fixed", 0, "<keyword>", "Positioning scheme. absolute places the box against its nearest positioned ancestor." },
            { "top", "auto", LengthPercent, "<length> | <percent> | auto", "Offset from the containing block's top edge." },
            { "right", "auto", LengthPercent, "<length> | <percent> | auto", "Offset from the containing block's right edge." },
            { "bottom", "auto", LengthPercent, "<length> | <percent> | auto", "Offset from the containing block's bottom edge." },
            { "left", "auto", LengthPercent, "<length> | <percent> | auto", "Offset from the containing block's left edge." },
            { "inset", "auto", LengthPercent, "<top> [<right> [<bottom> [<left>]]]", "Shorthand for top, right, bottom and left." },
            { "float", "none left right", 0, "<keyword>", "Floats the box to one side so content wraps around it." },
            { "clear", "none left right both", 0, "<keyword>", "Moves the box below earlier floats." },
            { "box-sizing", "content-box border-box", 0, "<keyword>", "Whether width and height include padding and border." },
            { "z-index", "auto", Number, "<number> | auto", "Stacking order among positioned siblings." },
            { "width", "auto", LengthPercent, "<length> | <percent> | auto", "Content width, or border box width under box-sizing: border-box." },
            { "min-width", "", LengthPercent, "<length> | <percent>", "Smallest width the box may shrink to." },
            { "max-width", "none", LengthPercent, "<length> | <percent> | none", "Largest width the box may grow to." },
            { "height", "auto", LengthPercent, "<length> | <percent> | auto", "Content height, or border box height under box-sizing: border-box." },
            { "min-height", "", LengthPercent, "<length> | <percent>", "Smallest height the box may shrink to." },
            { "max-height", "none", LengthPercent, "<length> | <percent> | none", "Largest height the box may grow to." },
            { "margin", "auto", LengthPercent, "<top> [<right> [<bottom> [<left>]]]", "Space outside the border. auto on both sides centers a block." },
            { "margin-top", "auto", LengthPercent, "<length> | <percent> | auto", "Space above the border." },
            { "margin-right", "auto", LengthPercent, "<length> | <percent> | auto", "Space right of the border." },
            { "margin-bottom", "auto", LengthPercent, "<length> | <percent> | auto", "Space below the border." },
            { "margin-left", "auto", LengthPercent, "<length> | <percent> | auto", "Space left of the border." },
            { "padding", "", LengthPercent, "<top> [<right> [<bottom> [<left>]]]", "Space between the border and the content." },
            { "padding-top", "", LengthPercent, "<length> | <percent>", "Space between the top border and the content." },
            { "padding-right", "", LengthPercent, "<length> | <percent>", "Space between the right border and the content." },
            { "padding-bottom", "", LengthPercent, "<length> | <percent>", "Space between the bottom border and the content." },
            { "padding-left", "", LengthPercent, "<length> | <percent>", "Space between the left border and the content." },
            { "border", "", Length | Color, "<width> <color>", "Width and color of all four borders. RmlUi borders are always solid." },
            { "border-top", "", Length | Color, "<width> <color>", "Width and color of the top border." },
            { "border-right", "", Length | Color, "<width> <color>", "Width and color of the right border." },
            { "border-bottom", "", Length | Color, "<width> <color>", "Width and color of the bottom border." },
            { "border-left", "", Length | Color, "<width> <color>", "Width and color of the left border." },
            { "border-width", "", Length, "<top> [<right> [<bottom> [<left>]]]", "Width of each border." },
            { "border-top-width", "", Length, "<length>", "Width of the top border." },
            { "border-right-width", "", Length, "<length>", "Width of the right border." },
            { "border-bottom-width", "", Length, "<length>", "Width of the bottom border." },
            { "border-left-width", "", Length, "<length>", "Width of the left border." },
            { "border-color", "", Color, "<top> [<right> [<bottom> [<left>]]]", "Color of each border." },
            { "border-top-color", "", Color, "<color>", "Color of the top border." },
            { "border-right-color", "", Color, "<color>", "Color of the right border." },
            { "border-bottom-color", "", Color, "<color>", "Color of the bottom border." },
            { "border-left-color", "", Color, "<color>", "Color of the left border." },
            { "border-radius", "", Length, "<top-left> [<top-right> [<bottom-right> [<bottom-left>]]]", "Rounds the corners of the background, border and clipping." },
            { "border-top-left-radius", "", Length, "<length>", "Radius of the top left corner." },
            { "border-top-right-radius", "", Length, "<length>", "Radius of the top right corner." },
            { "border-bottom-right-radius", "", Length, "<length>", "Radius of the bottom right corner." },
            { "border-bottom-left-radius", "", Length, "<length>", "Radius of the bottom left corner." },
            { "line-height", "", Number | LengthPercent, "<number> | <length> | <percent>", "Height of each line box. A bare number scales the font size." },
            { "vertical-align", "baseline middle sub super text-top text-bottom top center bottom", LengthPercent, "<keyword> | <length>", "Vertical position of an inline box or table cell." },
            { "overflow", "visible hidden auto scroll", 0, "<x> [<y>]", "What happens to content past the box. auto and scroll add scrollbars." },
            { "overflow-x", "visible hidden auto scroll", 0, "<keyword>", "Horizontal overflow behavior." },
            { "overflow-y", "visible hidden auto scroll", 0, "<keyword>", "Vertical overflow behavior." },
            { "clip", "auto none always", Number, "<keyword> | <number>", "Whether children are clipped, or how many ancestors to ignore when clipping." },
            { "visibility", "visible hidden", 0, "<keyword>", "Hides the box while it keeps its layout space." },
            { "text-overflow", "clip ellipsis", String, "<keyword> | <string>", "What marks text cut off by overflow." },
            { "background", "", Color, "<color>", "Shorthand for background-color. Images come from the decorator property." },
            { "background-color", "transparent", Color, "<color>", "Fill color behind the content and padding." },
            { "color", "", Color, "<color>", "Text color, inherited by children." },
            { "caret-color", "auto", Color, "<color> | auto", "Color of the text cursor in text fields." },
            { "image-color", "", Color, "<color>", "Tint multiplied into an <img> or image decorator." },
            { "opacity", "", Number, "<number>", "Opacity from 0 to 1, applied to the element and its children." },
            { "font", "normal italic bold", Length | String, "<style> <weight> <size> <family>", "Shorthand for font-style, font-weight, font-size and font-family." },
            { "font-family", "", String, "<string>", "Name of a registered font family." },
            { "font-style", "normal italic", 0, "<keyword>", "Upright or italic face." },
            { "font-weight", "normal bold", Number, "<keyword> | <number>", "Weight of the face, normal is 400 and bold is 700." },
            { "font-size", "", LengthPercent, "<length> | <percent>", "Size of the font." },
            { "font-kerning", "auto normal none", 0, "<keyword>", "Whether kerning tables are applied." },
            { "letter-spacing", "normal", Length, "<length> | normal", "Extra space between characters." },
            { "text-align", "left right center justify", 0, "<keyword>", "Horizontal alignment of inline content." },
            { "text-decoration", "none underline overline line-through", 0, "<keyword>", "Line drawn with the text." },
            { "text-transform", "none capitalize uppercase lowercase", 0, "<keyword>", "Changes the case of displayed text." },
            { "white-space", "normal pre nowrap pre-wrap pre-line", 0, "<keyword>", "How spaces and line breaks in the source are kept." },
            { "word-break", "normal break-all break-word", 0, "<keyword>", "Where long words may break." },
            { "gap", "", LengthPercent, "<row> [<column>]", "Space between flex rows and columns." },
            { "row-gap", "", LengthPercent, "<length> | <percent>", "Space between flex rows." },
            { "column-gap", "", LengthPercent, "<length> | <percent>", "Space between flex columns." },
            { "flex", "none auto", Number | LengthPercent, "<grow> [<shrink>] [<basis>]", "Shorthand for flex-grow, flex-shrink and flex-basis." },
            { "flex-flow", "row row-reverse column column-reverse nowrap wrap wrap-reverse", 0, "<direction> [<wrap>]", "Shorthand for flex-direction and flex-wrap." },
            { "flex-direction", "row row-reverse column column-reverse", 0, "<keyword>", "Main axis children are laid out along." },
            { "flex-wrap", "nowrap wrap wrap-reverse", 0, "<keyword>", "Whether children wrap onto more lines." },
            { "flex-grow", "", Number, "<number>", "Share of the leftover space this child takes." },
            { "flex-shrink", "", Number, "<number>", "Share of the overflow this child gives up." },
            { "flex-basis", "auto", LengthPercent, "<length> | <percent> | auto", "Starting size along the main axis before growing or shrinking." },
            { "justify-content", "flex-start flex-end center space-between space-around space-evenly", 0, "<keyword>", "Distributes children along the main axis." },
            { "align-items", "flex-start flex-end center baseline stretch", 0, "<keyword>", "Aligns children across the cross axis." },
            { "align-self", "auto flex-start flex-end center baseline stretch", 0, "<keyword>", "Overrides align-items for this child." },
            { "align-content", "flex-start flex-end center space-between space-around space-evenly stretch", 0, "<keyword>", "Distributes wrapped lines across the cross axis." },
            { "cursor", "", String, "<string>", "Mouse cursor name passed to the host while hovering." },
            { "drag", "none drag drag-drop block clone", 0, "<keyword>", "Whether the element can be dragged, and how." },
            { "tab-index", "none auto", 0, "<keyword>", "Whether Tab can move focus to this element." },
            { "focus", "none auto", 0, "<keyword>", "Whether the element can take focus." },
            { "nav", "none auto horizontal vertical tree-order", String, "<up> [<right> [<down> [<left>]]]", "Shorthand for the four nav directions used by gamepad and keyboard navigation." },
            { "nav-up", "none auto horizontal vertical tree-order", String, "<keyword> | <id>", "Element focused when navigating up." },
            { "nav-right", "none auto horizontal vertical tree-order", String, "<keyword> | <id>", "Element focused when navigating right." },
            { "nav-down", "none auto horizontal vertical tree-order", String, "<keyword> | <id>", "Element focused when navigating down." },
            { "nav-left", "none auto horizontal vertical tree-order", String, "<keyword> | <id>", "Element focused when navigating left." },
            { "scrollbar-margin", "", Length, "<length>", "Space kept clear between the content and the scrollbar." },
            { "overscroll-behavior", "auto contain", 0, "<keyword>", "Whether scrolling past the end passes to the parent." },
            { "pointer-events", "none auto", 0, "<keyword>", "none lets the mouse pass through to what is behind." },
            { "perspective", "none", Length, "<length> | none", "Distance to the viewer for 3D transforms of children." },
            { "perspective-origin", "left center right top bottom", LengthPercent, "<x> [<y>]", "Vanishing point for perspective." },
            { "perspective-origin-x", "left center right", LengthPercent, "<keyword> | <length>", "Horizontal vanishing point." },
            { "perspective-origin-y", "top center bottom", LengthPercent, "<keyword> | <length>", "Vertical vanishing point." },
            { "transform", "none", Transform, "<function>+ | none", "Translates, rotates, scales or skews the element without affecting layout." },
            { "transform-origin", "left center right top bottom", LengthPercent, "<x> [<y> [<z>]]", "Pivot point of the transform." },
            { "transform-origin-x", "left center right", LengthPercent, "<keyword> | <length>", "Horizontal pivot of the transform." },
            { "transform-origin-y", "top center bottom", LengthPercent, "<keyword> | <length>", "Vertical pivot of the transform." },
            { "transform-origin-z", "", Length, "<length>", "Depth pivot of the transform." },
            { "transition", "none all", Time | Tween | Property, "<property>+ <duration> [<tween>] [<delay>]", "Animates the named properties whenever they change, for example color 0.2s cubic-out." },
            { "animation", "none infinite alternate paused", Time | Tween | Number, "<duration> [<delay>] [<tween>] [<count> | infinite] [alternate] [paused] <keyframes>", "Plays an @keyframes animation." },
            { "decorator", "none", Decorator, "<decorator> [, <decorator>]*", "Draws backgrounds, images and gradients behind the content." },
            { "mask-image", "none", Decorator, "<decorator> [, <decorator>]*", "Masks the element by a decorator's alpha." },
            { "font-effect", "none", FontEffect, "<effect> [, <effect>]*", "Text effects such as outline, shadow, glow and blur." },
            { "filter", "none", Filter, "<filter>+", "Filters the element after it is drawn, such as blur or brightness." },
            { "backdrop-filter", "none", Filter, "<filter>+", "Filters what is behind the element before it is drawn." },
            { "box-shadow", "none inset", Shadow | Length | Color, "[inset] <x> <y> [<blur> [<spread>]] <color>", "Shadow cast by the border box." },
            { "fill-image", "", String, "<string>", "Image a progress bar fills with." },
            { "-rmlui-language", "", String, "<string>", "Language tag used for text shaping." },
            { "-rmlui-direction", "auto ltr rtl", 0, "<keyword>", "Text direction used for shaping." },
        };

        constexpr FRmlFunction FunctionTable[] =
        {
            { "image", "image(src [fill | contain | cover | scale-none | scale-down] [left | center | right] [top | center | bottom])", "Draws an image, sprite or UI material, fit and aligned within the box.", Decorator },
            { "ninepatch", "ninepatch(outer-sprite inner-sprite [edge-size])", "Stretches the center of a sprite while keeping its corners.", Decorator },
            { "tiled-horizontal", "tiled-horizontal(left-sprite center-sprite right-sprite)", "Caps on both ends with a stretched center, horizontally.", Decorator },
            { "tiled-vertical", "tiled-vertical(top-sprite center-sprite bottom-sprite)", "Caps on both ends with a stretched center, vertically.", Decorator },
            { "tiled-box", "tiled-box(top-left top-right bottom-left bottom-right left right top bottom center)", "Nine images forming a stretchable box.", Decorator },
            { "text", "text(\"text\" [color] [x y])", "Draws a string as a decoration.", Decorator },
            { "gradient", "gradient(direction start-color stop-color)", "Two color gradient, where direction is horizontal or vertical.", Decorator },
            { "horizontal-gradient", "horizontal-gradient(start-color stop-color)", "Left to right two color gradient.", Decorator },
            { "vertical-gradient", "vertical-gradient(start-color stop-color)", "Top to bottom two color gradient.", Decorator },
            { "linear-gradient", "linear-gradient(angle | to side, color-stop, color-stop)", "CSS linear gradient with any number of color stops.", Decorator },
            { "repeating-linear-gradient", "repeating-linear-gradient(angle | to side, color-stop, color-stop)", "Linear gradient that repeats its stops.", Decorator },
            { "radial-gradient", "radial-gradient(shape size at position, color-stop, color-stop)", "CSS radial gradient.", Decorator },
            { "repeating-radial-gradient", "repeating-radial-gradient(shape size at position, color-stop, color-stop)", "Radial gradient that repeats its stops.", Decorator },
            { "conic-gradient", "conic-gradient(from angle at position, color-stop, color-stop)", "CSS conic gradient around a center point.", Decorator },
            { "repeating-conic-gradient", "repeating-conic-gradient(from angle at position, color-stop, color-stop)", "Conic gradient that repeats its stops.", Decorator },
            { "outline", "outline(width color)", "Outlines each glyph.", FontEffect },
            { "shadow", "shadow(offset-x offset-y color)", "Drops a shadow behind the text.", FontEffect },
            { "glow", "glow(width [blur] offset-x offset-y color)", "Soft glow around the text.", FontEffect },
            { "blur", "blur(radius)", "Gaussian blur, of the text in font-effect or of the element in filter.", FontEffect | Filter },
            { "brightness", "brightness(amount)", "Scales brightness, 1 leaves it unchanged.", Filter },
            { "contrast", "contrast(amount)", "Scales contrast, 1 leaves it unchanged.", Filter },
            { "grayscale", "grayscale(amount)", "Desaturates toward gray, 1 is fully gray.", Filter },
            { "hue-rotate", "hue-rotate(angle)", "Rotates hues by an angle.", Filter },
            { "invert", "invert(amount)", "Inverts colors, 1 is fully inverted.", Filter },
            { "opacity", "opacity(amount)", "Fades the element, 1 leaves it opaque.", Filter },
            { "saturate", "saturate(amount)", "Scales saturation, 1 leaves it unchanged.", Filter },
            { "sepia", "sepia(amount)", "Tints toward sepia, 1 is fully sepia.", Filter },
            { "drop-shadow", "drop-shadow(color offset-x offset-y [blur])", "Shadow following the element's alpha.", Filter },
            { "translate", "translate(x, y)", "Moves the element.", Transform },
            { "translateX", "translateX(x)", "Moves the element horizontally.", Transform },
            { "translateY", "translateY(y)", "Moves the element vertically.", Transform },
            { "translateZ", "translateZ(z)", "Moves the element in depth.", Transform },
            { "translate3d", "translate3d(x, y, z)", "Moves the element in 3D.", Transform },
            { "scale", "scale(x, y)", "Scales the element.", Transform },
            { "scaleX", "scaleX(x)", "Scales the element horizontally.", Transform },
            { "scaleY", "scaleY(y)", "Scales the element vertically.", Transform },
            { "scaleZ", "scaleZ(z)", "Scales the element in depth.", Transform },
            { "scale3d", "scale3d(x, y, z)", "Scales the element in 3D.", Transform },
            { "rotate", "rotate(angle)", "Rotates the element in the plane, for example 45deg.", Transform },
            { "rotateX", "rotateX(angle)", "Rotates the element around the horizontal axis.", Transform },
            { "rotateY", "rotateY(angle)", "Rotates the element around the vertical axis.", Transform },
            { "rotateZ", "rotateZ(angle)", "Rotates the element around the depth axis.", Transform },
            { "rotate3d", "rotate3d(x, y, z, angle)", "Rotates the element around an arbitrary axis.", Transform },
            { "skew", "skew(x-angle, y-angle)", "Skews the element.", Transform },
            { "skewX", "skewX(angle)", "Skews the element horizontally.", Transform },
            { "skewY", "skewY(angle)", "Skews the element vertically.", Transform },
            { "perspective", "perspective(length)", "Applies perspective to this element's transform.", Transform },
            { "matrix", "matrix(a, b, c, d, e, f)", "2D transformation matrix.", Transform },
            { "matrix3d", "matrix3d(a1, b1, c1, d1, a2, b2, c2, d2, a3, b3, c3, d3, a4, b4, c4, d4)", "4x4 transformation matrix.", Transform },
            { "rgb", "rgb(red, green, blue)", "Color from 0 to 255 channels or percentages.", Color },
            { "rgba", "rgba(red, green, blue, alpha)", "Color with alpha. RmlUi reads alpha from 0 to 255, or as a percentage, unlike CSS.", Color },
            { "hsl", "hsl(hue, saturation%, lightness%)", "Color from hue in degrees with percentage saturation and lightness.", Color },
            { "hsla", "hsla(hue, saturation%, lightness%, alpha)", "hsl() with alpha from 0 to 1.", Color },
            { "lab", "lab(lightness a b [/ alpha])", "CIE Lab color.", Color },
            { "lch", "lch(lightness chroma hue [/ alpha])", "CIE LCH color.", Color },
            { "oklab", "oklab(lightness a b [/ alpha])", "Oklab perceptual color.", Color },
            { "oklch", "oklch(lightness chroma hue [/ alpha])", "Oklch perceptual color.", Color },
            { "var", "var(--name [, fallback])", "Value of a custom property declared with --name.", Length | Percent | Number | Color | String | Decorator | FontEffect | Filter | Transform },
        };

        constexpr FRmlNamed PseudoClassTable[] =
        {
            { "hover", "The mouse is over the element or one of its children." },
            { "active", "The element is being pressed." },
            { "focus", "The element has input focus." },
            { "focus-visible", "The element has focus that came from the keyboard or a gamepad." },
            { "checked", "A checkbox, radio button or option that is selected." },
            { "disabled", "A control with the disabled attribute." },
            { "selected", "The selected option of a select, or the active tab." },
            { "drag", "The element is being dragged." },
            { "placeholder-shown", "A text field showing its placeholder." },
            { "first-child", "The first child of its parent." },
            { "last-child", "The last child of its parent." },
            { "only-child", "The only child of its parent." },
            { "first-of-type", "The first sibling of its tag." },
            { "last-of-type", "The last sibling of its tag." },
            { "only-of-type", "The only sibling of its tag." },
            { "nth-child", "Children at positions matching an+b, such as 2n+1 or odd.", "nth-child(${1:2n+1})$0" },
            { "nth-last-child", "Children counted from the end matching an+b.", "nth-last-child(${1:1})$0" },
            { "nth-of-type", "Siblings of the same tag matching an+b.", "nth-of-type(${1:2n})$0" },
            { "nth-last-of-type", "Siblings of the same tag counted from the end.", "nth-last-of-type(${1:1})$0" },
            { "empty", "An element with no children." },
            { "not", "Elements that do not match the selector inside.", "not(${1:.class})$0" },
            { "scope", "The element a query starts from." },
        };

        constexpr FRmlNamed AtRuleTable[] =
        {
            { "keyframes", "Named animation steps for the animation property.", "keyframes ${1:name} {\n\tfrom { $2 }\n\tto { $0 }\n}" },
            { "media", "Rules that apply only when the context matches, such as a minimum width.", "media (${1:min-width: 800px}) {\n\t$0\n}" },
            { "spritesheet", "Names regions of an image as sprites for img and decorators.", "spritesheet ${1:name} {\n\tsrc: ${2:image.png};\n\t${3:sprite}: ${4:0px 0px 32px 32px};$0\n}" },
            { "decorator", "Names a configured decorator that rules can then use by name.", "decorator ${1:name} : ${2:linear-gradient} {\n\t$0\n}" },
            { "font-face", "Registers a font file under a family name.", "font-face {\n\tfont-family: ${1:Name};\n\tsrc: ${2:font.ttf};\n}" },
        };

        constexpr FRmlNamed UnitTable[] =
        {
            { "dp", "Density independent pixels, scaled by the context's dp ratio. The usual unit for UI." },
            { "px", "Physical pixels." },
            { "em", "Multiple of the element's font size." },
            { "rem", "Multiple of the root element's font size." },
            { "%", "Percentage of the containing block or of the inherited value." },
            { "vw", "Percentage of the context's width." },
            { "vh", "Percentage of the context's height." },
            { "s", "Seconds." },
            { "ms", "Milliseconds." },
            { "deg", "Degrees." },
            { "rad", "Radians." },
            { "x", "Resolution multiplier, used by @spritesheet resolution." },
            { "in", "Inches at 96 pixels each." },
            { "cm", "Centimeters." },
            { "mm", "Millimeters." },
            { "pt", "Points, 1/72 inch." },
            { "pc", "Picas, 12 points." },
        };

        constexpr FRmlNamed EntityTable[] =
        {
            { "lt;", "Less than sign." },
            { "gt;", "Greater than sign." },
            { "amp;", "Ampersand." },
            { "quot;", "Double quote." },
            { "nbsp;", "Non-breaking space." },
        };

        constexpr FRmlNamedColor ColorTable[] =
        {
            { "black", 0, 0, 0 },
            { "silver", 192, 192, 192 },
            { "gray", 128, 128, 128 },
            { "white", 255, 255, 255 },
            { "maroon", 128, 0, 0 },
            { "red", 255, 0, 0 },
            { "orange", 255, 165, 0 },
            { "purple", 128, 0, 128 },
            { "fuchsia", 255, 0, 255 },
            { "green", 0, 128, 0 },
            { "lime", 0, 255, 0 },
            { "olive", 128, 128, 0 },
            { "yellow", 255, 255, 0 },
            { "navy", 0, 0, 128 },
            { "blue", 0, 0, 255 },
            { "teal", 0, 128, 128 },
            { "aqua", 0, 255, 255 },
            { "transparent", 0, 0, 0, 0 },
        };

        constexpr const char* TweenTable[] =
        {
            "linear-in", "linear-out", "linear-in-out",
            "quadratic-in", "quadratic-out", "quadratic-in-out",
            "cubic-in", "cubic-out", "cubic-in-out",
            "quartic-in", "quartic-out", "quartic-in-out",
            "quintic-in", "quintic-out", "quintic-in-out",
            "sine-in", "sine-out", "sine-in-out",
            "circular-in", "circular-out", "circular-in-out",
            "exponential-in", "exponential-out", "exponential-in-out",
            "back-in", "back-out", "back-in-out",
            "bounce-in", "bounce-out", "bounce-in-out",
            "elastic-in", "elastic-out", "elastic-in-out",
        };

        constexpr FRmlNamed DocumentSnippetTable[] =
        {
            { "Document skeleton", "A complete document with head, inline style and body.",
              "<rml>\n<head>\n\t<title>${1:Untitled}</title>\n\t<style>\n\t\tbody { color: #fff; }\n\t</style>\n</head>\n<body>\n\t$0\n</body>\n</rml>\n" },
            { "Inline <style> block", "A <style> element for RCSS inside the document.", "<style>\n\t$0\n</style>\n" },
            { "<link rel=\"stylesheet\">", "Links an external .rcss file.", "<link rel=\"stylesheet\" type=\"text/rcss\" href=\"${1:style.rcss}\"/>\n$0" },
            { "<link type=\"text/template\">", "Registers a template file for template= and <template src>.", "<link type=\"text/template\" href=\"${1:widget.rml}\"/>\n$0" },
            { "<div id=...>", "A container with an id and class.", "<div id=\"$1\" class=\"$2\">\n\t$0\n</div>\n" },
            { "<button>", "A button with a click handler.", "<button onclick=\"$1\">${2:Label}</button>\n$0" },
            { "<input text>", "A single line text field.", "<input type=\"text\" name=\"$1\" value=\"$2\"/>\n$0" },
            { "<input checkbox>", "A checkbox.", "<input type=\"checkbox\" name=\"$1\" checked/>\n$0" },
            { "<input range>", "A slider.", "<input type=\"range\" name=\"$1\" min=\"${2:0}\" max=\"${3:1}\" step=\"${4:0.1}\" value=\"${5:0.5}\"/>\n$0" },
            { "<select / option>", "A drop-down with two options.", "<select name=\"$1\">\n\t<option value=\"a\">A</option>\n\t<option value=\"b\">B</option>\n</select>\n$0" },
            { "<tabset / panel>", "Two tabs with their panels.", "<tabset>\n\t<tab>${1:One}</tab>\n\t<panel>\n\t\t$0\n\t</panel>\n\t<tab>${2:Two}</tab>\n\t<panel>\n\t</panel>\n</tabset>\n" },
            { "<progress>", "A progress bar.", "<progress value=\"${1:0.5}\" max=\"1.0\"/>\n$0" },
            { "<handle> (drag)", "A handle that drags its parent.", "<handle move_target=\"#parent\">${1:drag}</handle>\n$0" },
            { "<template> include", "Places a registered template.", "<template src=\"$1\"/>\n$0" },
            { "data-for list", "Repeats an element for each item of a model list.", "<div data-for=\"${1:item} : ${2:items}\">{{ $3 }}</div>\n$0" },
        };

        constexpr FRmlNamed StyleSnippetTable[] =
        {
            { "Centered flex container", "Centers children on both axes.", "display: flex;\njustify-content: center;\nalign-items: center;\n$0" },
            { "Vertical flex column", "Stacks children with a gap.", "display: flex;\nflex-direction: column;\ngap: ${1:8dp};\n$0" },
            { "Absolute fill", "Covers the positioned parent.", "position: absolute;\nleft: 0; top: 0; right: 0; bottom: 0;\n$0" },
            { "Border and radius", "A thin rounded border.", "border: ${1:1dp} ${2:#444};\nborder-radius: ${3:4dp};\n$0" },
            { "Transition", "Animates color changes.", "transition: ${1:background-color color} ${2:0.15s} ${3:cubic-out};\n$0" },
            { "Gradient decorator", "A vertical two color gradient.", "decorator: vertical-gradient(${1:#1f2a36} ${2:#0e1620});\n$0" },
            { "Drop shadow text", "A soft text shadow.", "font-effect: shadow(${1:0dp} ${2:1dp} ${3:#000a});\n$0" },
            { "Outline text", "Outlined text.", "font-effect: outline(${1:1dp} ${2:#000});\n$0" },
        };
    }

    using namespace RmlKnowledgeData;

    TSpan<const FRmlElement> Elements() { return ElementTable; }
    TSpan<const FRmlAttribute> Attributes() { return AttributeTable; }
    TSpan<const FRmlNamed> Events() { return EventTable; }
    TSpan<const FRmlProperty> Properties() { return PropertyTable; }
    TSpan<const FRmlFunction> Functions() { return FunctionTable; }
    TSpan<const FRmlNamed> PseudoClasses() { return PseudoClassTable; }
    TSpan<const FRmlNamed> AtRules() { return AtRuleTable; }
    TSpan<const FRmlNamed> Units() { return UnitTable; }
    TSpan<const FRmlNamed> Entities() { return EntityTable; }
    TSpan<const FRmlNamedColor> Colors() { return ColorTable; }
    TSpan<const char* const> Tweens() { return TweenTable; }
    TSpan<const FRmlNamed> DocumentSnippets() { return DocumentSnippetTable; }
    TSpan<const FRmlNamed> StyleSnippets() { return StyleSnippetTable; }

    const FRmlElement* FindElement(FStringView Name)
    {
        for (const FRmlElement& Element : ElementTable)
        {
            if (EqualsIgnoreCase(Name, FStringView(Element.Name)))
            {
                return &Element;
            }
        }
        return nullptr;
    }

    const FRmlAttribute* FindAttribute(FStringView Element, FStringView Name)
    {
        const FRmlAttribute* Global = nullptr;
        for (const FRmlAttribute& Attribute : AttributeTable)
        {
            if (!EqualsIgnoreCase(Name, FStringView(Attribute.Name)))
            {
                continue;
            }
            if (*Attribute.Element == '\0')
            {
                Global = Global != nullptr ? Global : &Attribute;
            }
            else if (EqualsIgnoreCase(Element, FStringView(Attribute.Element)))
            {
                return &Attribute;
            }
        }
        return Global;
    }

    const FRmlNamed* FindNamed(TSpan<const FRmlNamed> Table, FStringView Name)
    {
        for (const FRmlNamed& Entry : Table)
        {
            if (Name == FStringView(Entry.Name))
            {
                return &Entry;
            }
        }
        return nullptr;
    }

    bool ContainsWord(const char* SpaceSeparated, FStringView Word)
    {
        const FStringView List(SpaceSeparated);
        size_t Start = 0;
        while (Start < List.size())
        {
            size_t End = List.find(' ', Start);
            End = End == FStringView::npos ? List.size() : End;
            if (List.substr(Start, End - Start) == Word)
            {
                return true;
            }
            Start = End + 1;
        }
        return false;
    }
}
