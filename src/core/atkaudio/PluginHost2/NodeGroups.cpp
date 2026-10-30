#include "NodeGroups.h"
#include <algorithm>

namespace
{
bool includes(const std::vector<juce::uint32>& values, juce::uint32 value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::unique_ptr<juce::XmlElement> writeGroup(const NodeGroups& groups, const NodeGroup& group)
{
    auto xml = std::make_unique<juce::XmlElement>("GROUP");
    xml->setAttribute("id", (int)group.id);
    xml->setAttribute("name", group.name);
    xml->setAttribute("bypassed", group.bypassed);
    xml->setAttribute("collapsed", group.collapsed);
    for (auto& member : group.members)
    {
        auto* child = xml->createNewChildElement("MEMBER");
        child->setAttribute("uid", (int)member.uid);
        child->setAttribute("bypassBeforeGroup", member.bypassBeforeGroup);
    }
    for (auto& state : group.bypassStates)
    {
        auto* child = xml->createNewChildElement("BYPASS_STATE");
        child->setAttribute("uid", (int)state.uid);
        child->setAttribute("bypassed", state.bypassBeforeGroup);
    }
    for (auto& child : group.childGroups)
        if (auto* nested = groups.find(child.groupId))
            xml->addChildElement(writeGroup(groups, *nested).release());
    return xml;
}
} // namespace

bool NodeGroup::contains(juce::uint32 uid) const
{
    return std::any_of(members.begin(), members.end(), [uid](const Member& member) { return member.uid == uid; });
}

NodeGroup* NodeGroups::find(juce::uint32 id)
{
    for (auto& group : groups)
        if (group.id == id)
            return &group;
    return nullptr;
}

const NodeGroup* NodeGroups::find(juce::uint32 id) const
{
    return const_cast<NodeGroups*>(this)->find(id);
}

const NodeGroup* NodeGroups::findGroupOf(juce::uint32 uid) const
{
    for (auto& group : groups)
        if (group.contains(uid))
            return &group;
    return nullptr;
}

const NodeGroup* NodeGroups::findParentOf(juce::uint32 id) const
{
    for (auto& group : groups)
        for (auto& child : group.childGroups)
            if (child.groupId == id)
                return &group;
    return nullptr;
}

void NodeGroups::collectNodeUids(const NodeGroup& group, std::vector<juce::uint32>& uids) const
{
    for (auto& member : group.members)
        uids.push_back(member.uid);
    for (auto& child : group.childGroups)
        if (auto* nested = find(child.groupId))
            collectNodeUids(*nested, uids);
}

std::vector<juce::uint32> NodeGroups::getNodeUids(juce::uint32 id) const
{
    std::vector<juce::uint32> result;
    if (auto* group = find(id))
        collectNodeUids(*group, result);
    return result;
}

std::vector<juce::uint32> NodeGroups::getGroupIdsWithin(const std::vector<juce::uint32>& uids) const
{
    std::vector<juce::uint32> result;
    for (auto& group : groups)
    {
        auto descendants = getNodeUids(group.id);
        if (!descendants.empty()
            && std::all_of(
                descendants.begin(),
                descendants.end(),
                [&uids](juce::uint32 uid) { return includes(uids, uid); }
            ))
            result.push_back(group.id);
    }
    auto selected = result;
    result.erase(
        std::remove_if(
            result.begin(),
            result.end(),
            [this, &selected](juce::uint32 id)
            {
                for (auto* parent = findParentOf(id); parent != nullptr; parent = findParentOf(parent->id))
                    if (includes(selected, parent->id))
                        return true;
                return false;
            }
        ),
        result.end()
    );
    return result;
}

NodeGroups::CreateResult NodeGroups::create(
    const std::vector<juce::uint32>& uids,
    const std::vector<juce::uint32>& groupIds,
    const juce::String& name
)
{
    std::vector<juce::uint32> selectedGroups;
    for (auto id : groupIds)
        if (find(id) != nullptr && !includes(selectedGroups, id))
            selectedGroups.push_back(id);
    auto allGroups = selectedGroups;
    selectedGroups.erase(
        std::remove_if(
            selectedGroups.begin(),
            selectedGroups.end(),
            [this, &allGroups](juce::uint32 id)
            {
                for (auto* parent = findParentOf(id); parent != nullptr; parent = findParentOf(parent->id))
                    if (includes(allGroups, parent->id))
                        return true;
                return false;
            }
        ),
        selectedGroups.end()
    );
    std::vector<juce::uint32> selectedNodes;
    for (auto uid : uids)
        if (uid != 0 && !includes(selectedNodes, uid))
            selectedNodes.push_back(uid);
    for (auto id : selectedGroups)
    {
        auto descendants = getNodeUids(id);
        selectedNodes.erase(
            std::remove_if(
                selectedNodes.begin(),
                selectedNodes.end(),
                [&descendants](juce::uint32 uid) { return includes(descendants, uid); }
            ),
            selectedNodes.end()
        );
    }
    if (selectedNodes.size() + selectedGroups.size() < 2)
        return {};
    CreateResult result;
    for (auto uid : selectedNodes)
    {
        auto released = removeNode(uid);
        result.released.insert(result.released.end(), released.begin(), released.end());
    }
    for (auto id : selectedGroups)
    {
        auto* parent = const_cast<NodeGroup*>(findParentOf(id));
        if (parent == nullptr)
            continue;
        auto descendants = getNodeUids(id);
        std::vector<juce::uint32> ancestors;
        juce::uint32 bypassedAncestor = 0;
        for (auto* ancestor = parent; ancestor != nullptr;
             ancestor = const_cast<NodeGroup*>(findParentOf(ancestor->id)))
        {
            ancestors.push_back(ancestor->id);
            if (ancestor->bypassed)
                bypassedAncestor = ancestor->id;
        }
        if (auto* ancestor = find(bypassedAncestor))
            for (auto uid : descendants)
            {
                auto saved = std::find_if(
                    ancestor->bypassStates.begin(),
                    ancestor->bypassStates.end(),
                    [uid](const NodeGroup::Member& state) { return state.uid == uid; }
                );
                result.released.push_back(
                    {uid, saved != ancestor->bypassStates.end() ? saved->bypassBeforeGroup : false}
                );
            }
        for (auto ancestorId : ancestors)
            if (auto* ancestor = find(ancestorId))
                ancestor->bypassStates.erase(
                    std::remove_if(
                        ancestor->bypassStates.begin(),
                        ancestor->bypassStates.end(),
                        [&descendants](const NodeGroup::Member& state) { return includes(descendants, state.uid); }
                    ),
                    ancestor->bypassStates.end()
                );
        auto parentId = parent->id;
        parent->childGroups.erase(
            std::remove_if(
                parent->childGroups.begin(),
                parent->childGroups.end(),
                [id](const NodeGroup::Child& child) { return child.groupId == id; }
            ),
            parent->childGroups.end()
        );
        if (parent->size() < 2)
        {
            auto released = ungroup(parentId);
            result.released.insert(result.released.end(), released.begin(), released.end());
        }
    }
    NodeGroup group;
    group.id = ++lastId;
    group.name = name;
    for (auto uid : selectedNodes)
        group.members.push_back({uid, false});
    for (auto id : selectedGroups)
        group.childGroups.push_back({id});
    groups.push_back(std::move(group));
    result.groupId = lastId;
    return result;
}

std::vector<NodeGroups::ReleasedNode> NodeGroups::ungroup(juce::uint32 id)
{
    auto* existing = find(id);
    if (existing == nullptr)
        return {};
    auto group = *existing;
    auto* parent = const_cast<NodeGroup*>(findParentOf(id));
    auto parentId = parent != nullptr ? parent->id : 0;
    std::vector<ReleasedNode> released;
    auto uids = getNodeUids(id);
    for (auto uid : uids)
    {
        if (parent != nullptr && (parent->bypassed || !group.bypassed))
            continue;
        auto state = std::find_if(
            group.bypassStates.begin(),
            group.bypassStates.end(),
            [uid](const NodeGroup::Member& saved) { return saved.uid == uid; }
        );
        auto member = std::find_if(
            group.members.begin(),
            group.members.end(),
            [uid](const NodeGroup::Member& saved) { return saved.uid == uid; }
        );
        bool restore = state != group.bypassStates.end() ? state->bypassBeforeGroup
                     : member != group.members.end()     ? member->bypassBeforeGroup
                                                         : false;
        released.push_back({uid, group.bypassed ? std::optional<bool>(restore) : std::nullopt});
    }
    if (parent != nullptr)
    {
        auto child = std::find_if(
            parent->childGroups.begin(),
            parent->childGroups.end(),
            [id](const NodeGroup::Child& entry) { return entry.groupId == id; }
        );
        auto index = std::distance(parent->childGroups.begin(), child);
        parent->childGroups.erase(child);
        parent->members.insert(parent->members.end(), group.members.begin(), group.members.end());
        parent->childGroups
            .insert(parent->childGroups.begin() + index, group.childGroups.begin(), group.childGroups.end());
    }
    groups.erase(
        std::remove_if(groups.begin(), groups.end(), [id](const NodeGroup& entry) { return entry.id == id; }),
        groups.end()
    );
    parent = find(parentId);
    if (parent != nullptr && parent->size() < 2)
    {
        auto additional = ungroup(parentId);
        released.insert(released.end(), additional.begin(), additional.end());
    }
    return released;
}

std::vector<NodeGroups::ReleasedNode> NodeGroups::removeNode(juce::uint32 uid)
{
    auto* group = const_cast<NodeGroup*>(findGroupOf(uid));
    if (group == nullptr)
        return {};
    auto id = group->id;
    std::vector<ReleasedNode> released;
    bool restore = false;
    for (auto& member : group->members)
        if (member.uid == uid)
            restore = member.bypassBeforeGroup;
    for (auto& state : group->bypassStates)
        if (state.uid == uid)
            restore = state.bypassBeforeGroup;
    released.push_back({uid, group->bypassed ? std::optional<bool>(restore) : std::nullopt});
    group->members.erase(
        std::remove_if(
            group->members.begin(),
            group->members.end(),
            [uid](const NodeGroup::Member& member) { return member.uid == uid; }
        ),
        group->members.end()
    );
    group->bypassStates.erase(
        std::remove_if(
            group->bypassStates.begin(),
            group->bypassStates.end(),
            [uid](const NodeGroup::Member& member) { return member.uid == uid; }
        ),
        group->bypassStates.end()
    );
    if (group->size() < 2)
    {
        auto additional = ungroup(id);
        released.insert(released.end(), additional.begin(), additional.end());
    }
    return released;
}

void NodeGroups::clear()
{
    groups.clear();
    lastId = 0;
}

std::unique_ptr<juce::XmlElement> NodeGroups::createXml() const
{
    auto xml = std::make_unique<juce::XmlElement>(xmlTag);
    for (auto& group : groups)
        if (findParentOf(group.id) == nullptr)
            xml->addChildElement(writeGroup(*this, group).release());
    return xml;
}

std::unique_ptr<juce::XmlElement> NodeGroups::createXmlForGroupsWithin(const std::vector<juce::uint32>& uids) const
{
    auto xml = std::make_unique<juce::XmlElement>(xmlTag);
    for (auto id : getGroupIdsWithin(uids))
        xml->addChildElement(writeGroup(*this, *find(id)).release());
    return xml;
}

bool NodeGroups::appendTree(const juce::XmlElement& xml, std::vector<NodeGroup>& target, juce::uint32 parentId)
{
    NodeGroup group;
    group.id = (juce::uint32)xml.getIntAttribute("id");
    group.name = xml.getStringAttribute("name");
    group.bypassed = xml.getBoolAttribute("bypassed");
    group.collapsed = xml.getBoolAttribute("collapsed");
    if (group.id == 0
        || std::any_of(target.begin(), target.end(), [&group](const NodeGroup& other) { return other.id == group.id; }))
        return false;
    for (auto* child : xml.getChildWithTagNameIterator("MEMBER"))
    {
        auto uid = (juce::uint32)child->getIntAttribute("uid");
        if (uid == 0
            || group.contains(uid)
            || std::any_of(target.begin(), target.end(), [uid](const NodeGroup& other) { return other.contains(uid); }))
            return false;
        group.members.push_back({uid, child->getBoolAttribute("bypassBeforeGroup")});
    }
    for (auto* child : xml.getChildWithTagNameIterator("BYPASS_STATE"))
        group.bypassStates.push_back({(juce::uint32)child->getIntAttribute("uid"), child->getBoolAttribute("bypassed")});
    auto index = target.size();
    auto id = group.id;
    for (auto& parent : target)
        if (parent.id == parentId)
            parent.childGroups.push_back({id});
    target.push_back(std::move(group));
    for (auto* child : xml.getChildWithTagNameIterator("GROUP"))
        if (!appendTree(*child, target, id))
            return false;
    if (target[index].size() < 2)
        return false;
    std::vector<juce::uint32> descendants, saved;
    for (size_t position = index; position < target.size(); ++position)
        for (auto& member : target[position].members)
            descendants.push_back(member.uid);
    for (auto& state : target[index].bypassStates)
    {
        if (!includes(descendants, state.uid) || includes(saved, state.uid))
            return false;
        saved.push_back(state.uid);
    }
    return saved.empty() || saved.size() == descendants.size();
}

void NodeGroups::restoreFromXml(const juce::XmlElement& xml)
{
    clear();
    for (auto* child : xml.getChildWithTagNameIterator("GROUP"))
    {
        std::vector<NodeGroup> tree;
        if (!appendTree(*child, tree, 0))
            continue;
        bool overlaps = false;
        for (auto& group : tree)
        {
            overlaps = overlaps || find(group.id) != nullptr;
            for (auto& member : group.members)
                overlaps = overlaps || findGroupOf(member.uid) != nullptr;
        }
        if (overlaps)
            continue;
        for (auto& group : tree)
        {
            lastId = std::max(lastId, group.id);
            groups.push_back(std::move(group));
        }
    }
}

std::vector<juce::uint32>
NodeGroups::addFromXml(const juce::XmlElement& xml, const std::map<juce::uint32, juce::uint32>& uidMap)
{
    NodeGroups source;
    source.restoreFromXml(xml);
    std::vector<juce::uint32> added;
    for (auto& root : source.getAll())
    {
        if (source.findParentOf(root.id) != nullptr)
            continue;
        auto uids = source.getNodeUids(root.id);
        std::vector<juce::uint32> mapped;
        bool valid = true;
        for (auto uid : uids)
        {
            auto found = uidMap.find(uid);
            if (found == uidMap.end()
                || found->second == 0
                || findGroupOf(found->second) != nullptr
                || includes(mapped, found->second))
            {
                valid = false;
                break;
            }
            mapped.push_back(found->second);
        }
        if (!valid)
            continue;
        auto treeXml = writeGroup(source, root);
        std::vector<NodeGroup> tree;
        source.appendTree(*treeXml, tree, 0);
        std::map<juce::uint32, juce::uint32> ids;
        for (auto& group : tree)
            ids[group.id] = ++lastId;
        for (auto& group : tree)
        {
            group.id = ids.at(group.id);
            for (auto& child : group.childGroups)
                child.groupId = ids.at(child.groupId);
            for (auto& member : group.members)
                member.uid = uidMap.at(member.uid);
            for (auto& state : group.bypassStates)
                state.uid = uidMap.at(state.uid);
            added.push_back(group.id);
            groups.push_back(std::move(group));
        }
    }
    return added;
}
