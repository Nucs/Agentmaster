// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — STATE FILE COMPATIBILITY across versions (doc/agentmaster/PERSISTENCE.md §17; the owner's task of
// 2026-10-28: "making sure that any case will always migrate successfully without hurting the existing files and their
// data").
//
// Every Agentmaster state file (sessions.json, settings.json, the window records, …) is written by serializing the build's
// own model of it. A build therefore DROPS what it does not model: the fields a NEWER build added, a value it cannot
// represent (a newer enum word), a record it cannot parse. Before this header, opening a profile with an older build and
// saving once lost all of that for good. Two mechanisms, both pure JSON so the m5 harness tests every rule:
//
//   1. THE RESIDUE (forward compatibility — this build and every later one). Before a save, the file on disk is compared
//      with what THIS build makes of it (its round trip: Serialize(Deserialize(disk))). What the round trip loses is the
//      residue (ExtractResidue): members it neither writes nor even looks at (LookedKeys: a member the loader asked for is
//      one this build understands, such as a consumed migration key), values it writes differently, array elements it
//      drops. The save puts the residue back into the fresh document (ApplyResidue): a member the fresh document lacks is
//      added back; a value this build could not represent is restored while this build has not changed it; a dropped
//      element is appended. What this build DOES understand is never touched: a member it writes, and a member it changed
//      since, are always the fresh ones, so a user's change in this build is never undone.
//
//   2. THE OLDER-WRITER REPAIR (backward: builds released before this header). Such a build cannot be changed, and it
//      drops our fields when it saves. Every document this build writes carries a WRITER STAMP (the root member
//      "writer", written first), and a SHADOW copy of what it wrote is kept beside the profile (StateBackups.h). A
//      stamp-less file was therefore rewritten by an older build since; RestoreKeysDroppedByOlderWriter puts back, from
//      the shadow, the members that build PROVABLY did not know — never a member it knew and may have reset on purpose:
//        * inside a collection (an array of records with an identity: the sessions, a session's queue, the tabs), a
//          member is restored only when NO record of that collection in the file has it (the writer had no such column);
//        * outside a collection, only when THIS build writes the member unconditionally (it is in the default document's
//          template), so its absence cannot be a value left out because it was the default;
//        * never a member the caller names as volatile (a draft, a one-shot flag): one cleared on every record by a
//          writer that knew it reads exactly like one the writer never knew, and bringing it back would act again.
//
// Neither mechanism touches the root "version" (each build writes its own format version; a file whose version is newer
// than the build's is not written at all — the caller's format guard) or the root "writer" (each build stamps its own).

#pragma once

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Json.h"

namespace Agentmaster::StateCompat
{
    /// <summary>The root member that carries the writer stamp (WriterStamp). Its presence marks a file written by a build
    /// that keeps unknown fields; its absence, a file an older build rewrote.</summary>
    inline constexpr std::wstring_view kWriterKey = L"writer";
    /// <summary>The root member that carries a document's format version, which every build writes as its own.</summary>
    inline constexpr std::wstring_view kVersionKey = L"version";

    /// <summary>
    /// The members that name an array element, tried in this order: the first one holding a non-empty string is the
    /// element's identity. "id" (sessions, prompts, presets), "sessionId" (topics, Claude tabs), "windowId", "dir" (folder
    /// colors, folder environments), "name" (templates), "actionsJson" (shell tabs, whose saved actions are unique).
    /// </summary>
    inline constexpr std::wstring_view kIdentityKeys[] = { L"id", L"sessionId", L"windowId", L"dir", L"name", L"actionsJson" };

    /// <summary>
    /// The identity of an array element: its first identity member (kIdentityKeys) that holds a non-empty string, as
    /// "<key>&#x1F;<value>" so two different keys can never collide.
    /// </summary>
    /// <param name="e">The element.</param>
    /// <returns>The identity; "" when the element is not an object or has none.</returns>
    /// <exception cref="std::bad_alloc">The identity string could not be built.</exception>
    inline std::wstring ElementIdentity(const json::Value& e)
    {
        if (e.type != json::Value::Type::Obj)
        {
            return {};
        }
        for (const auto key : kIdentityKeys)
        {
            if (const auto* v = e.Find(key); v && v->type == json::Value::Type::Str && !v->str.empty())
            {
                return std::wstring{ key } + L'\x1F' + v->str;
            }
        }
        return {};
    }

    /// <summary>
    /// Whether an array can be matched element by element: every element is an object with an identity, and no two share
    /// one. An empty array qualifies (there is nothing to match). Arrays that do not qualify are compared whole.
    /// </summary>
    /// <param name="a">The array.</param>
    /// <returns>True when identities can match its elements.</returns>
    /// <exception cref="std::bad_alloc">The identity set could not grow.</exception>
    inline bool IdentityUsable(const json::Value& a)
    {
        if (a.type != json::Value::Type::Arr)
        {
            return false;
        }
        std::set<std::wstring> seen;
        for (const auto& e : a.arr)
        {
            auto id = ElementIdentity(e);
            if (id.empty() || !seen.insert(std::move(id)).second)
            {
                return false;
            }
        }
        return true;
    }

    /// <summary>
    /// JSON equality: same type and same value; arrays element by element in order; objects as unordered maps (the same
    /// keys with equal values, whatever the member order). Numbers compare as the doubles the parser holds.
    /// </summary>
    /// <param name="a">One value.</param>
    /// <param name="b">The other value.</param>
    /// <returns>True when they are equal.</returns>
    /// <remarks>Never throws (no allocation: lookups only).</remarks>
    inline bool Equal(const json::Value& a, const json::Value& b) noexcept
    {
        if (a.type != b.type)
        {
            return false;
        }
        switch (a.type)
        {
        case json::Value::Type::Null:
            return true;
        case json::Value::Type::Bool:
            return a.boolean == b.boolean;
        case json::Value::Type::Num:
            return a.num == b.num;
        case json::Value::Type::Str:
            return a.str == b.str;
        case json::Value::Type::Arr:
            if (a.arr.size() != b.arr.size())
            {
                return false;
            }
            for (size_t i = 0; i < a.arr.size(); ++i)
            {
                if (!Equal(a.arr[i], b.arr[i]))
                {
                    return false;
                }
            }
            return true;
        case json::Value::Type::Obj:
        default:
            if (a.members.size() != b.members.size())
            {
                return false;
            }
            for (const auto& [k, v] : a.members)
            {
                const auto* other = b.Find(k);
                if (!other || !Equal(v, *other))
                {
                    return false;
                }
            }
            return true;
        }
    }

    /// <summary>A member of an object, by key, for writing: the first member with that key, or nullptr.</summary>
    /// <param name="o">The object.</param>
    /// <param name="key">The member's key.</param>
    /// <returns>The member's value, or nullptr (also when <paramref name="o"/> is not an object).</returns>
    /// <remarks>Never throws.</remarks>
    inline json::Value* FindMutable(json::Value& o, std::wstring_view key) noexcept
    {
        if (o.type != json::Value::Type::Obj)
        {
            return nullptr;
        }
        for (auto& [k, v] : o.members)
        {
            if (k == key)
            {
                return &v;
            }
        }
        return nullptr;
    }

    /// <summary>
    /// The members a loader LOOKED AT while it read a document — the residue's evidence of what this build understands.
    /// Installed with ObserveLookups around the loader's run over the parsed file (Json.h detail::FindObserver records
    /// every Find on the thread). A member the loader asked for is understood even when the build then leaves it out of
    /// what it writes: a consumed migration key (launch-presets.json's "seededTemplates", read only to run a one-time
    /// cleanup) must not be kept, or the cleanup would run again at every load. Only members the loader never asked for
    /// (a newer build's) are residue.
    /// </summary>
    /// <remarks>
    /// ⚠ A loader that finds a key by iterating `members` itself is not observed: what it then does not write counts as
    /// unknown and is kept. A member a newer build changed the TYPE of is looked at here and read as a default, so it is
    /// not kept — a newer build must add a new key rather than retype an old one.
    /// </remarks>
    class LookedKeys final : public json::detail::FindObserver
    {
    public:
        /// <summary>Records one lookup (Json.h calls it from Value::Find on the observing thread).</summary>
        /// <param name="object">The value Find was called on.</param>
        /// <param name="key">The key asked for (copied).</param>
        /// <remarks>Never throws: a record that cannot be stored marks the evidence incomplete (Complete()).</remarks>
        void OnFind(const json::Value& object, std::wstring_view key) noexcept override
        {
            try
            {
                _byObject[&object].emplace(key);
            }
            catch (...)
            {
                _complete = false; // out of memory: the evidence is partial, so the caller must not rely on it
            }
        }

        /// <summary>Whether the loader asked an object for a key.</summary>
        /// <param name="object">The object (by address: the very value of the document the loader read).</param>
        /// <param name="key">The key.</param>
        /// <returns>True when it did.</returns>
        /// <remarks>Never throws (lookups only).</remarks>
        bool Looked(const json::Value& object, std::wstring_view key) const noexcept
        {
            const auto it = _byObject.find(&object);
            return it != _byObject.end() && it->second.find(key) != it->second.end();
        }

        /// <summary>Whether every lookup was recorded (false after an allocation failure).</summary>
        /// <returns>True when the evidence is complete.</returns>
        bool Complete() const noexcept { return _complete; }

    private:
        // Object → the keys asked of it. std::less<> lets Looked search by string_view without a copy.
        std::unordered_map<const json::Value*, std::set<std::wstring, std::less<>>> _byObject;
        bool _complete{ true };
    };

    /// <summary>
    /// RAII: makes <paramref name="keys"/> this thread's lookup observer for the scope's life, then puts the previous one
    /// back. Wrap exactly the loader's run over the parsed document (and nothing that reads another document of the same
    /// addresses).
    /// </summary>
    class ObserveLookups
    {
    public:
        /// <summary>Installs the observer.</summary>
        /// <param name="keys">Receives every lookup made on this thread until the scope ends.</param>
        explicit ObserveLookups(LookedKeys& keys) noexcept :
            _previous(json::detail::t_findObserver)
        {
            json::detail::t_findObserver = &keys;
        }

        /// <summary>Restores the previous observer (normally none).</summary>
        ~ObserveLookups() { json::detail::t_findObserver = _previous; }

        ObserveLookups(const ObserveLookups&) = delete;
        ObserveLookups& operator=(const ObserveLookups&) = delete;

    private:
        json::detail::FindObserver* _previous;
    };

    struct ResidueNode;
    struct ArrayResidue;

    /// <summary>One member of the residue of an object: what this build's round trip lost at that member.</summary>
    struct ResidueEntry
    {
        /// <summary>How the round trip lost it.</summary>
        enum class Kind
        {
            Missing, // the round trip did not write the member: put it back when the fresh document lacks it
            Object, // both sides are objects: descend (the residue below it is in `object`)
            Array, // both sides are identity-matched arrays: descend element by element (`array`)
            Changed, // the round trip wrote another value: restore `value` while the fresh one still equals `seen`
        };
        /// <summary>The kind of loss.</summary>
        Kind kind{ Kind::Missing };
        /// <summary>Missing: the member's value as the file had it. Changed: the file's value.</summary>
        json::Value value;
        /// <summary>Changed: the value this build's round trip wrote instead (what "unchanged since" compares against).</summary>
        json::Value seen;
        /// <summary>Object: the residue of the nested object.</summary>
        std::shared_ptr<ResidueNode> object;
        /// <summary>Array: the residue of the identity-matched array.</summary>
        std::shared_ptr<ArrayResidue> array;
    };

    /// <summary>The residue of one object: its lost members, in the file's member order.</summary>
    struct ResidueNode
    {
        /// <summary>Member key → what was lost there.</summary>
        std::vector<std::pair<std::wstring, ResidueEntry>> entries;
        /// <summary>Whether nothing was lost here (nor anywhere below).</summary>
        /// <returns>True when there is no entry.</returns>
        bool empty() const noexcept { return entries.empty(); }
    };

    /// <summary>The residue of an identity-matched array.</summary>
    struct ArrayResidue
    {
        /// <summary>Element identity (ElementIdentity) → the residue of that element, for the elements that lost something.</summary>
        std::vector<std::pair<std::wstring, ResidueNode>> byId;
        /// <summary>Whole elements the round trip dropped (this build could not represent them), in file order.</summary>
        std::vector<json::Value> dropped;
        /// <summary>Whether nothing was lost in the array.</summary>
        /// <returns>True when no element lost anything and none was dropped.</returns>
        bool empty() const noexcept { return byId.empty() && dropped.empty(); }
    };

    namespace detail
    {
        inline ResidueNode ExtractObject(const json::Value& original, const json::Value* roundTrip, bool isRoot, bool keepDropped, const LookedKeys* looked);

        /// <summary>The residue of two identity-matched arrays (both IdentityUsable).</summary>
        /// <param name="original">The file's array.</param>
        /// <param name="roundTrip">This build's round trip of it.</param>
        /// <param name="keepDropped">Whether an element the round trip dropped whole is kept (see ExtractResidue).</param>
        /// <param name="looked">The loader's lookups over <paramref name="original"/>'s document, or nullptr (see ExtractResidue).</param>
        /// <returns>The residue: per-element losses, and the elements the round trip dropped.</returns>
        /// <exception cref="std::bad_alloc">The residue could not be built.</exception>
        inline ArrayResidue ExtractArray(const json::Value& original, const json::Value& roundTrip, bool keepDropped, const LookedKeys* looked)
        {
            ArrayResidue out;
            std::map<std::wstring, const json::Value*> byId;
            for (const auto& e : roundTrip.arr)
            {
                byId.emplace(ElementIdentity(e), &e);
            }
            for (const auto& oe : original.arr)
            {
                auto id = ElementIdentity(oe);
                const auto it = byId.find(id);
                if (it == byId.end())
                {
                    if (keepDropped)
                    {
                        out.dropped.push_back(oe); // the round trip lost the whole element: this build could not represent it
                    }
                    continue;
                }
                auto child = ExtractObject(oe, it->second, false, keepDropped, looked);
                if (!child.empty())
                {
                    out.byId.emplace_back(std::move(id), std::move(child));
                }
            }
            return out;
        }

        /// <summary>The residue of one object (see ExtractResidue for the rules).</summary>
        /// <param name="original">The file's object.</param>
        /// <param name="roundTrip">This build's round trip of it; nullptr or a non-object = it wrote nothing here.</param>
        /// <param name="isRoot">Whether this is the document root (whose "version" and "writer" are never residue).</param>
        /// <param name="keepDropped">Whether an array element the round trip dropped whole is kept (see ExtractResidue).</param>
        /// <param name="looked">The loader's lookups over <paramref name="original"/>'s document, or nullptr (see ExtractResidue).</param>
        /// <returns>The residue.</returns>
        /// <exception cref="std::bad_alloc">The residue could not be built.</exception>
        inline ResidueNode ExtractObject(const json::Value& original, const json::Value* roundTrip, bool isRoot, bool keepDropped, const LookedKeys* looked)
        {
            ResidueNode out;
            if (original.type != json::Value::Type::Obj)
            {
                return out;
            }
            const json::Value* rt = (roundTrip && roundTrip->type == json::Value::Type::Obj) ? roundTrip : nullptr;
            for (const auto& [k, ov] : original.members)
            {
                if (isRoot && (k == kVersionKey || k == kWriterKey))
                {
                    continue; // each build writes its own format version and its own stamp
                }
                const json::Value* rv = rt ? rt->Find(k) : nullptr;
                ResidueEntry e;
                if (!rv)
                {
                    if (looked && looked->Looked(original, k))
                    {
                        // The loader asked for it and the build chose not to write it: understood, not unknown — a value
                        // it read as its default and leaves out, or a consumed migration key whose presence would run the
                        // migration again at every load. Dropped, as every build before this header dropped it.
                        continue;
                    }
                    // Never even looked at: a newer build's member (or an old key this build no longer reads at all).
                    // Kept: dropping it is the one thing that loses data; keeping it costs a key this build ignores.
                    e.kind = ResidueEntry::Kind::Missing;
                    e.value = ov;
                }
                else if (ov.type == json::Value::Type::Obj && rv->type == json::Value::Type::Obj)
                {
                    auto child = ExtractObject(ov, rv, false, keepDropped, looked);
                    if (child.empty())
                    {
                        continue;
                    }
                    e.kind = ResidueEntry::Kind::Object;
                    e.object = std::make_shared<ResidueNode>(std::move(child));
                }
                else if (ov.type == json::Value::Type::Arr && rv->type == json::Value::Type::Arr && IdentityUsable(ov) && IdentityUsable(*rv))
                {
                    auto arr = ExtractArray(ov, *rv, keepDropped, looked);
                    if (arr.empty())
                    {
                        continue;
                    }
                    e.kind = ResidueEntry::Kind::Array;
                    e.array = std::make_shared<ArrayResidue>(std::move(arr));
                }
                else if (!Equal(ov, *rv))
                {
                    if (!keepDropped && ov.type == json::Value::Type::Arr)
                    {
                        // An array matched whole (its elements have no usable identities, or duplicate ones) in a file
                        // whose loader drops elements on purpose: restoring the file's array would put the dropped ones
                        // back and undo that migration. This build's array stands.
                        continue;
                    }
                    // A value this build could not represent (a newer enum word, a precision it rounds, an array whose
                    // elements it filters): kept as the file had it while this build does not change it.
                    e.kind = ResidueEntry::Kind::Changed;
                    e.value = ov;
                    e.seen = *rv;
                }
                else
                {
                    continue; // understood and written exactly: nothing lost
                }
                out.entries.emplace_back(k, std::move(e));
            }
            return out;
        }

        inline ResidueNode ApplyObject(json::Value& fresh, const ResidueNode& residue);

        /// <summary>Puts an array's residue back into the fresh array.</summary>
        /// <param name="fresh">The fresh array (this build's serialization of its current data).</param>
        /// <param name="residue">The array's residue.</param>
        /// <returns>The part of the residue the array now carries (see ApplyResidue).</returns>
        /// <exception cref="std::bad_alloc">The array could not grow.</exception>
        inline ArrayResidue ApplyArray(json::Value& fresh, const ArrayResidue& residue)
        {
            ArrayResidue carried;
            if (fresh.type != json::Value::Type::Arr)
            {
                return carried;
            }
            std::map<std::wstring, size_t> index;
            for (size_t i = 0; i < fresh.arr.size(); ++i)
            {
                if (auto id = ElementIdentity(fresh.arr[i]); !id.empty())
                {
                    index.emplace(std::move(id), i);
                }
            }
            for (const auto& [id, child] : residue.byId)
            {
                if (const auto it = index.find(id); it != index.end())
                {
                    if (auto applied = ApplyObject(fresh.arr[it->second], child); !applied.empty())
                    {
                        carried.byId.emplace_back(id, std::move(applied));
                    }
                }
                // An element this build removed since the file was written (a closed record, a sent prompt): its residue
                // goes with it — never resurrected.
            }
            // Appended only after the walk above: push_back may move the elements, and the index holds positions.
            for (const auto& d : residue.dropped)
            {
                // An element this build could not represent: it never showed it, so it can never have removed it. Put
                // back unless an element with its identity exists now (this build made its own since; it wins).
                if (auto id = ElementIdentity(d); id.empty() || index.find(id) == index.end())
                {
                    fresh.arr.push_back(d);
                    carried.dropped.push_back(d);
                }
            }
            return carried;
        }

        /// <summary>Puts an object's residue back into the fresh object (see ApplyResidue for the rules).</summary>
        /// <param name="fresh">The fresh object.</param>
        /// <param name="residue">Its residue.</param>
        /// <returns>The part of the residue the object now carries (see ApplyResidue).</returns>
        /// <exception cref="std::bad_alloc">The object could not grow.</exception>
        inline ResidueNode ApplyObject(json::Value& fresh, const ResidueNode& residue)
        {
            ResidueNode carried;
            if (fresh.type != json::Value::Type::Obj)
            {
                return carried;
            }
            for (const auto& [k, e] : residue.entries)
            {
                // Looked up per entry: a Set below may move the members, so no pointer is kept across entries.
                json::Value* fv = FindMutable(fresh, k);
                switch (e.kind)
                {
                case ResidueEntry::Kind::Missing:
                    if (!fv)
                    {
                        fresh.Set(k, e.value);
                        carried.entries.emplace_back(k, e);
                    }
                    // else: this build writes the member now (a newer member it learned, or one it set since) — its
                    // value is the file's from here on, and the entry is spent.
                    break;
                case ResidueEntry::Kind::Object:
                    if (fv && e.object)
                    {
                        if (auto applied = ApplyObject(*fv, *e.object); !applied.empty())
                        {
                            ResidueEntry c;
                            c.kind = ResidueEntry::Kind::Object;
                            c.object = std::make_shared<ResidueNode>(std::move(applied));
                            carried.entries.emplace_back(k, std::move(c));
                        }
                    }
                    break;
                case ResidueEntry::Kind::Array:
                    if (fv && e.array)
                    {
                        if (auto applied = ApplyArray(*fv, *e.array); !applied.empty())
                        {
                            ResidueEntry c;
                            c.kind = ResidueEntry::Kind::Array;
                            c.array = std::make_shared<ArrayResidue>(std::move(applied));
                            carried.entries.emplace_back(k, std::move(c));
                        }
                    }
                    break;
                case ResidueEntry::Kind::Changed:
                default:
                    // Only while this build has not changed it: a fresh value that differs from what the round trip wrote
                    // is this build's own change (or a default it now leaves out — fv null), and wins; the entry is then
                    // spent, so a later change back to the old value is never mistaken for "unchanged".
                    if (fv && Equal(*fv, e.seen))
                    {
                        *fv = e.value;
                        carried.entries.emplace_back(k, e);
                    }
                    break;
                }
            }
            return carried;
        }

        /// <summary>Counts the entries of a residue, nested ones and dropped elements included (for logs and tests).</summary>
        /// <param name="r">The residue.</param>
        /// <returns>The count.</returns>
        /// <remarks>Never throws.</remarks>
        inline size_t CountObject(const ResidueNode& r) noexcept
        {
            size_t n = 0;
            for (const auto& [k, e] : r.entries)
            {
                if (e.kind == ResidueEntry::Kind::Object && e.object)
                {
                    n += CountObject(*e.object);
                }
                else if (e.kind == ResidueEntry::Kind::Array && e.array)
                {
                    n += e.array->dropped.size();
                    for (const auto& [id, child] : e.array->byId)
                    {
                        n += CountObject(child);
                    }
                }
                else
                {
                    ++n;
                }
            }
            return n;
        }
    }

    /// <summary>
    /// What a file holds that this build's round trip loses (forward compatibility, mechanism 1). Per member of each object:
    /// one the round trip does not write and the loader never looked at is kept whole (Missing); objects on both sides are
    /// compared member by member (Object); arrays whose elements carry identities (IdentityUsable) are matched by
    /// identity, element by element, and an element the round trip dropped is kept whole (Array); any other value the round
    /// trip wrote differently is kept with what it wrote instead (Changed). The root's "version" and "writer" are never
    /// residue.
    /// </summary>
    /// <param name="original">The document as the file holds it.</param>
    /// <param name="roundTrip">This build's round trip of that document: Serialize(Deserialize(file)).</param>
    /// <param name="keepDropped">
    /// Whether an array element the round trip drops WHOLE is kept (appended back at the save). True for every file whose
    /// loader drops only what it cannot represent; false for a file whose loader drops elements ON PURPOSE as a documented
    /// migration (launch-presets.json leaves out the untouched template presets of release 0.6.8.18, and the next save is
    /// meant to remove them) — keeping them would undo that migration at every save. With false, an array compared whole
    /// is never restored either (it would bring the dropped elements back the same way). Members of matched elements are
    /// kept either way.
    /// </param>
    /// <param name="looked">
    /// The lookups the loader made while the round trip read <paramref name="original"/> (LookedKeys, recorded over the
    /// very same document object), or nullptr. A member the round trip does not write but the loader asked for is
    /// understood, not unknown, and is not kept. nullptr (or incomplete evidence, the caller's call) keeps every such member.
    /// </param>
    /// <returns>The residue; empty when this build represents the whole file.</returns>
    /// <exception cref="std::bad_alloc">The residue could not be built.</exception>
    inline ResidueNode ExtractResidue(const json::Value& original, const json::Value& roundTrip, bool keepDropped = true, const LookedKeys* looked = nullptr)
    {
        return detail::ExtractObject(original, &roundTrip, true, keepDropped, looked);
    }

    /// <summary>
    /// Puts a residue back into a fresh document (this build's serialization of its current data) before it is saved. A
    /// Missing member is added only when the fresh document lacks it; a Changed value is restored only while the fresh
    /// value still equals what the round trip wrote (otherwise this build changed it, and the change wins); identity-
    /// matched elements get their own residue, an element this build removed since gets none, and a dropped element is
    /// appended when no element has its identity. A residue whose shape no longer fits (a member that is no longer an
    /// object or an array) is skipped there.
    /// </summary>
    /// <param name="fresh">The fresh document; receives the residue.</param>
    /// <param name="residue">The residue (ExtractResidue).</param>
    /// <returns>
    /// The CARRIED residue: the entries that took effect, i.e. the residue of the document as it will be written. After a
    /// successful write it replaces the file's residue, so an entry this save spent (a member this build now writes, a
    /// value it changed) never acts again — a later change back to the old value is then the user's, not "unchanged".
    /// Valid only when <paramref name="fresh"/> is this build's own serialization (members it does not understand are
    /// never in it); a caller writing any other document must have the file judged again instead.
    /// </returns>
    /// <exception cref="std::bad_alloc">The document could not grow (it may then hold part of the residue).</exception>
    inline ResidueNode ApplyResidue(json::Value& fresh, const ResidueNode& residue)
    {
        return detail::ApplyObject(fresh, residue);
    }

    /// <summary>The number of entries in a residue, nested ones and dropped array elements included.</summary>
    /// <param name="residue">The residue.</param>
    /// <returns>The count (0 = this build represents the whole file).</returns>
    /// <remarks>Never throws.</remarks>
    inline size_t CountResidue(const ResidueNode& residue) noexcept
    {
        return detail::CountObject(residue);
    }

    // ===== the writer stamp ====================================================================================

    /// <summary>
    /// The writer stamp a build puts at the root of every state document it writes (the root member "writer"): which
    /// build wrote it. Older builds do not know the member and drop it when they save — which is how a later build tells
    /// that an older one rewrote the file (HasWriterStamp).
    /// </summary>
    /// <param name="version">The build's version ("0.8.1.0").</param>
    /// <param name="install">The install kind ("release", "dev", "portable", "unpackaged").</param>
    /// <returns>The stamp object: {"app":"Agentmaster","version":…,"install":…}.</returns>
    /// <exception cref="std::bad_alloc">The stamp could not be built.</exception>
    inline json::Value WriterStamp(std::wstring_view version, std::wstring_view install)
    {
        auto o = json::Value::MkObj();
        o.Set(L"app", json::Value::MkStr(L"Agentmaster"));
        o.Set(L"version", json::Value::MkStr(std::wstring{ version }));
        o.Set(L"install", json::Value::MkStr(std::wstring{ install }));
        return o;
    }

    /// <summary>Whether a document carries a writer stamp (a build that keeps unknown fields wrote it last).</summary>
    /// <param name="doc">The document.</param>
    /// <returns>True when its root has a "writer" object.</returns>
    /// <remarks>Never throws.</remarks>
    inline bool HasWriterStamp(const json::Value& doc) noexcept
    {
        const auto* w = doc.type == json::Value::Type::Obj ? doc.Find(kWriterKey) : nullptr;
        return w && w->type == json::Value::Type::Obj;
    }

    /// <summary>
    /// Puts a writer stamp at the root of a document as its FIRST member, replacing any stamp it had. First, so a reader can
    /// tell a stamped file from its first characters (TextStartsWithWriterStamp) without parsing a 10 MB sessions.json.
    /// </summary>
    /// <param name="doc">The document (an object; anything else is left alone).</param>
    /// <param name="stamp">The stamp (WriterStamp).</param>
    /// <exception cref="std::bad_alloc">The document could not grow.</exception>
    inline void SetWriterStamp(json::Value& doc, const json::Value& stamp)
    {
        if (doc.type != json::Value::Type::Obj)
        {
            return;
        }
        auto& m = doc.members;
        m.erase(std::remove_if(m.begin(), m.end(), [](const auto& member) { return member.first == kWriterKey; }), m.end());
        m.insert(m.begin(), { std::wstring{ kWriterKey }, stamp });
    }

    /// <summary>
    /// Whether a document's text starts with the writer stamp: "{", then the "writer" key (whitespace and a leading U+FEFF
    /// allowed), as every document written through SetWriterStamp + json::Dump does. True proves the file is stamped; false
    /// proves nothing (a hand edit may have moved the member) — parse to be sure.
    /// </summary>
    /// <param name="text">The file's text.</param>
    /// <returns>True when the text starts with the stamp's key.</returns>
    /// <remarks>Never throws.</remarks>
    inline bool TextStartsWithWriterStamp(std::wstring_view text) noexcept
    {
        size_t i = 0;
        const auto skipBlank = [&] {
            while (i < text.size() && (text[i] == L' ' || text[i] == L'\t' || text[i] == L'\r' || text[i] == L'\n' || text[i] == static_cast<wchar_t>(0xFEFF)))
            {
                ++i;
            }
        };
        skipBlank();
        if (i >= text.size() || text[i] != L'{')
        {
            return false;
        }
        ++i;
        skipBlank();
        constexpr std::wstring_view kQuotedKey = L"\"writer\"";
        return text.substr(i, kQuotedKey.size()) == kQuotedKey;
    }

    /// <summary>
    /// Whether a file's format is NEWER than the build's: its root "version" is greater than the one the build writes. A
    /// build must not write such a file at all (the caller's format guard): it may be a format this build cannot
    /// represent, and its own version number would also tell the newer build to read the file as the older format.
    /// </summary>
    /// <param name="onDisk">The document on disk.</param>
    /// <param name="fresh">The build's fresh document (carries the version it writes).</param>
    /// <returns>True when both carry a numeric "version" and the file's is greater.</returns>
    /// <remarks>Never throws.</remarks>
    inline bool FormatIsNewer(const json::Value& onDisk, const json::Value& fresh) noexcept
    {
        const auto* dv = onDisk.type == json::Value::Type::Obj ? onDisk.Find(kVersionKey) : nullptr;
        const auto* fv = fresh.type == json::Value::Type::Obj ? fresh.Find(kVersionKey) : nullptr;
        return dv && fv && dv->type == json::Value::Type::Num && fv->type == json::Value::Type::Num && dv->num > fv->num;
    }

    // ===== the older-writer repair (mechanism 2) ===============================================================

    /// <summary>
    /// The keys present at each path of a collection, across ALL its records: the "columns" the file's writer wrote.
    /// Built from the records of one collection in the file (an identity-matched array), nested objects and nested
    /// arrays merged across records.
    /// </summary>
    struct ColumnKeys
    {
        /// <summary>The keys found at this level in any record.</summary>
        std::set<std::wstring> keys;
        /// <summary>Key → the columns of the objects found under it in any record.</summary>
        std::map<std::wstring, ColumnKeys> objects;
        /// <summary>Key → the columns of the elements of the arrays found under it in any record.</summary>
        std::map<std::wstring, ColumnKeys> arrays;

        /// <summary>Adds one record's keys (recursively) to the columns.</summary>
        /// <param name="o">The record (an object; anything else adds nothing).</param>
        /// <exception cref="std::bad_alloc">The sets could not grow.</exception>
        void Add(const json::Value& o)
        {
            if (o.type != json::Value::Type::Obj)
            {
                return;
            }
            for (const auto& [k, v] : o.members)
            {
                keys.insert(k);
                if (v.type == json::Value::Type::Obj)
                {
                    objects[k].Add(v);
                }
                else if (v.type == json::Value::Type::Arr)
                {
                    auto& col = arrays[k];
                    for (const auto& e : v.arr)
                    {
                        col.Add(e);
                    }
                }
            }
        }
    };

    namespace detail
    {
        /// <summary>
        /// Restores into `main` the members of `shadow` the file's writer provably did not know (see
        /// RestoreKeysDroppedByOlderWriter). One object, then recursively its objects and identity-matched arrays.
        /// </summary>
        /// <param name="main">An object of the file (the older writer's).</param>
        /// <param name="shadow">The same object in the shadow (what this build's family last wrote).</param>
        /// <param name="templ">The same object in the default document's template, or nullptr (inside a collection).</param>
        /// <param name="columns">The collection's columns at this path, or nullptr (outside any collection).</param>
        /// <param name="isRoot">Whether this is the document root ("version" / "writer" are never restored).</param>
        /// <param name="path">This object's path, for the report ("settings", "sessions[].autorunner").</param>
        /// <param name="neverRestore">Member paths that are never restored nor descended into, or nullptr (see
        /// RestoreKeysDroppedByOlderWriter).</param>
        /// <param name="restoredPaths">Receives "path.key" for every restored member.</param>
        /// <returns>The number of members restored here and below.</returns>
        /// <exception cref="std::bad_alloc">An object or the report could not grow.</exception>
        inline size_t RestoreObject(json::Value& main, const json::Value& shadow, const json::Value* templ, const ColumnKeys* columns, bool isRoot,
                                    const std::wstring& path, const std::set<std::wstring, std::less<>>* neverRestore, std::vector<std::wstring>& restoredPaths)
        {
            if (main.type != json::Value::Type::Obj || shadow.type != json::Value::Type::Obj)
            {
                return 0;
            }
            size_t restored = 0;
            for (const auto& [k, sv] : shadow.members)
            {
                if (isRoot && (k == kVersionKey || k == kWriterKey))
                {
                    continue;
                }
                const std::wstring childPath = path.empty() ? k : path + L"." + k;
                if (neverRestore && neverRestore->find(childPath) != neverRestore->end())
                {
                    continue; // volatile: absent may well mean "cleared by a writer that knew it"
                }
                const json::Value* tv = (templ && templ->type == json::Value::Type::Obj) ? templ->Find(k) : nullptr;
                json::Value* mv = FindMutable(main, k);
                if (!mv)
                {
                    // Did the writer know this member? Inside a collection: it did if ANY record of the file has it (a
                    // column it writes). Outside: only a member this build always writes (in the default document's
                    // template) proves anything — its absence cannot be a default that was left out. Anything else stays
                    // as the writer left it: a member it knew and may have reset or removed on purpose.
                    const bool writerDidNotKnow = columns ? columns->keys.find(k) == columns->keys.end() : tv != nullptr;
                    if (writerDidNotKnow)
                    {
                        main.Set(k, sv);
                        restoredPaths.push_back(childPath);
                        ++restored;
                    }
                    continue;
                }
                if (mv->type == json::Value::Type::Obj && sv.type == json::Value::Type::Obj)
                {
                    const ColumnKeys* childColumns = nullptr;
                    if (columns)
                    {
                        const auto it = columns->objects.find(k);
                        childColumns = it == columns->objects.end() ? nullptr : &it->second;
                        if (!childColumns)
                        {
                            continue; // inside a collection, but no record has an object here: nothing provable below
                        }
                    }
                    restored += RestoreObject(*mv, sv, (tv && tv->type == json::Value::Type::Obj) ? tv : nullptr, childColumns, false, childPath, neverRestore, restoredPaths);
                }
                else if (mv->type == json::Value::Type::Arr && sv.type == json::Value::Type::Arr && IdentityUsable(*mv) && IdentityUsable(sv))
                {
                    // A collection: its columns are the keys across ALL its records — across every record of the outer
                    // collection too, when this array sits inside one (a session's queue: every session's prompts).
                    ColumnKeys local;
                    const ColumnKeys* elementColumns = nullptr;
                    if (columns)
                    {
                        const auto it = columns->arrays.find(k);
                        elementColumns = it == columns->arrays.end() ? nullptr : &it->second;
                    }
                    else
                    {
                        for (const auto& e : mv->arr)
                        {
                            local.Add(e);
                        }
                        elementColumns = &local;
                    }
                    if (!elementColumns)
                    {
                        continue;
                    }
                    std::map<std::wstring, const json::Value*> shadowById;
                    for (const auto& e : sv.arr)
                    {
                        shadowById.emplace(ElementIdentity(e), &e);
                    }
                    for (auto& me : mv->arr)
                    {
                        if (const auto it = shadowById.find(ElementIdentity(me)); it != shadowById.end())
                        {
                            restored += RestoreObject(me, *it->second, nullptr, elementColumns, false, childPath + L"[]", neverRestore, restoredPaths);
                        }
                        // A record the writer added has no shadow; one it removed is not resurrected (it was the
                        // writer's own change, or its records are not ours to judge).
                    }
                }
                // Scalars, and arrays without identities: the writer's value stands (it knew the member).
            }
            return restored;
        }
    }

    /// <summary>
    /// The older-writer repair (mechanism 2): puts back into a file an older build rewrote the members it dropped, taken
    /// from the shadow of what a build with this header last wrote. Only members the older writer provably did not know
    /// are restored: inside a collection, a member no record of that collection carries; outside one, a member this build
    /// always writes (present in <paramref name="templ"/>). Records are matched by identity; a record the older writer
    /// added keeps what it has, one it removed stays removed; values the older writer wrote (it knew those members) are
    /// never replaced. The root's "version" and "writer" are never restored.
    /// </summary>
    /// <param name="main">The file's document (the older writer's); receives the restored members.</param>
    /// <param name="shadow">The shadow's document (what a build that keeps unknown fields last wrote).</param>
    /// <param name="templ">This build's serialization of a DEFAULT document of that file type: the members it always
    /// writes. A member that only a collection carries needs no template.</param>
    /// <param name="neverRestore">
    /// Member paths ("sessions[].pendingInput") that are never restored nor descended into, or nullptr. For VOLATILE
    /// members only — a draft, a one-shot flag, a member the app clears once it acted on it: the column rule cannot tell
    /// "a writer that knew it cleared it on every record" from "a writer that never knew it", and bringing such a member
    /// back would act again (a stale draft typed into a box, a fork stopped twice).
    /// </param>
    /// <param name="restoredPaths">Receives "path.key" for every restored member ("settings.voiceWorkerCommand",
    /// "sessions[].createdUnixMs"), for the log.</param>
    /// <returns>The number of members restored.</returns>
    /// <exception cref="std::bad_alloc">The document or the report could not grow.</exception>
    inline size_t RestoreKeysDroppedByOlderWriter(json::Value& main, const json::Value& shadow, const json::Value& templ,
                                                  const std::set<std::wstring, std::less<>>* neverRestore, std::vector<std::wstring>& restoredPaths)
    {
        return detail::RestoreObject(main, shadow, &templ, nullptr, true, L"", neverRestore, restoredPaths);
    }
}
