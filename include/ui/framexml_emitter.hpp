#pragma once

// Turns FrameXML into the Lua that builds it.
//
// The alternative was to build widgets from C++ directly, walking the XML and
// calling into the tree. That would have meant a second implementation of
// everything CreateFrame already does - parenting, naming, templates, script
// binding - kept in step with the first by hand. Emitting Lua instead means XML
// frames and hand-written frames travel the same path, so anything fixed for
// one is fixed for both, and a template defined in XML is usable from a script
// without translation.
//
// Emitting source rather than driving Lua directly also makes this testable
// without a Lua state: the output is a string, and a test can read it.

#include <string>
#include <vector>

namespace wowee {
namespace ui {

struct XmlNode;

struct EmitResult {
    std::string lua;
    /// Files named by <Script file="..."/> and <Include file="..."/>, in the
    /// order they appeared. The caller loads them; resolving paths is its job,
    /// not the emitter's.
    std::vector<std::string> scriptFiles;
    std::vector<std::string> includeFiles;
    std::vector<std::string> warnings;

    /// The document in the order the client processes it: each <Script> and
    /// <Include> runs where it stands, between the frames declared before and
    /// after it. A Lua step is the code for the elements in between, complete
    /// in itself. `lua`, `scriptFiles` and `includeFiles` above are the same
    /// content grouped by kind.
    ///
    /// The order is the addon's to rely on: ElvUI's Load_Config.xml runs
    /// Profile.lua and Global.lua and then <Include>s the filters, which fill
    /// tables Global.lua creates. Running every include before any script
    /// read those tables before they existed.
    struct Step {
        enum class Kind { Lua, Script, Include };
        Kind kind;
        std::string text;  ///< the Lua for a Lua step, the file name otherwise
    };
    std::vector<Step> steps;
};

/// Emit Lua for a parsed <Ui> document. `virtualPrefix` names the table that
/// virtual (template) frames are recorded in so later `inherits` can find them.
EmitResult emitFrameXml(const XmlNode& root);

/// Substitute FrameXML's $parent token. A child named "$parentText" inside
/// "FooFrame" becomes "FooFrameText"; this is how nearly every region in the
/// original interface is named.
std::string substituteParent(const std::string& name, const std::string& parentName);

} // namespace ui
} // namespace wowee
