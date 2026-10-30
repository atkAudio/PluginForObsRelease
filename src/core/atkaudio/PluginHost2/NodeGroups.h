#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <map>
#include <optional>
#include <vector>

struct NodeGroup
{
    struct Member
    {
        juce::uint32 uid = 0;
        bool bypassBeforeGroup = false;
    };

    struct Child
    {
        juce::uint32 groupId = 0;
    };

    juce::uint32 id = 0;
    juce::String name;
    std::vector<Member> members;
    std::vector<Child> childGroups;
    std::vector<Member> bypassStates;
    bool bypassed = false;
    bool collapsed = false;
    bool contains(juce::uint32 uid) const;

    size_t size() const
    {
        return members.size() + childGroups.size();
    }
};

class NodeGroups
{
public:
    struct ReleasedNode
    {
        juce::uint32 uid = 0;
        std::optional<bool> restoreBypass;
    };

    struct CreateResult
    {
        juce::uint32 groupId = 0;
        std::vector<ReleasedNode> released;
    };

    CreateResult create(const std::vector<juce::uint32>&, const std::vector<juce::uint32>&, const juce::String&);

    CreateResult create(const std::vector<juce::uint32>& nodes, const juce::String& name)
    {
        return create(nodes, {}, name);
    }

    std::vector<ReleasedNode> ungroup(juce::uint32);
    std::vector<ReleasedNode> removeNode(juce::uint32);
    NodeGroup* find(juce::uint32);
    const NodeGroup* find(juce::uint32) const;
    const NodeGroup* findGroupOf(juce::uint32) const;
    const NodeGroup* findParentOf(juce::uint32) const;
    std::vector<juce::uint32> getNodeUids(juce::uint32) const;
    std::vector<juce::uint32> getGroupIdsWithin(const std::vector<juce::uint32>&) const;

    const std::vector<NodeGroup>& getAll() const
    {
        return groups;
    }

    void clear();
    std::unique_ptr<juce::XmlElement> createXml() const;
    std::unique_ptr<juce::XmlElement> createXmlForGroupsWithin(const std::vector<juce::uint32>&) const;
    void restoreFromXml(const juce::XmlElement&);
    std::vector<juce::uint32> addFromXml(const juce::XmlElement&, const std::map<juce::uint32, juce::uint32>&);
    static constexpr const char* xmlTag = "GROUPS";

private:
    bool appendTree(const juce::XmlElement&, std::vector<NodeGroup>&, juce::uint32);
    void collectNodeUids(const NodeGroup&, std::vector<juce::uint32>&) const;
    std::vector<NodeGroup> groups;
    juce::uint32 lastId = 0;
};
