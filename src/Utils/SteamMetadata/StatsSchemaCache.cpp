#include "StatsSchemaCache.h"

#include "dllmain.h"
#include "Utils/Logging/Log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ════════════════════════════════════════════════════════════════
//  StatsSchemaCache
//
//  解析 Steam 客户端缓存的成就 schema（二进制 KeyValues），只为一件事：
//  数出成就总数（achievement bits 节点条目数），给 Player.GetAchievementsProgress
//  响应注入提供 total 字段。
//
//  二进制 KV 方言（已对本机 141 个 schema bin 全量验证，全部精确解析到 EOF）：
//    循环 { u8 type; 若 type==8(End) 结束当前容器; name = \0 结尾 UTF-8 串; value }
//    实际只出现三种类型：0=None(容器，递归) 1=String(\0结尾) 2=Int32(定长4字节)
//    另外按方言保守支持：3/6/7=定长4字节 4/5=定长8字节。
//
//  schema 结构：
//    <appid> { stats { <stat> { type, type_int, bits { <每个成就一项> } } } }
//    成就 stat 的识别：type 字符串为 ACHIEVEMENTS / GROUPACHIEVEMENTS，
//    或 type_int 为 4/5（UserStatType 的 Achievements/GroupAchievements）。
// ════════════════════════════════════════════════════════════════

namespace
{
    constexpr size_t kMaxSchemaFileBytes = 8 * 1024 * 1024;   // schema 都很小，超限当损坏处理
    constexpr int    kMaxDepth          = 24;
    constexpr size_t kMaxNameBytes      = 4096;

    enum class KVType : uint8_t
    {
        None       = 0,
        String     = 1,
        Int32      = 2,   // 本方言里 2 是定长 4 字节整数（实测 141 个文件验证）
        WideString = 3,   // 未在本方言中出现过，按 4 字节兜底
        Int64      = 4,   // 未出现过，按 8 字节兜底
        Float32    = 5,   // 未出现过，按 8 字节兜底
        Color      = 6,
        Pointer    = 7,
        End        = 8,
    };

    struct KVNode
    {
        std::string          name;
        KVType               type = KVType::None;
        std::string          stringValue;   // type == String
        int32_t              int32Value = 0; // type == Int32 系列；String 时为 atoi 结果
        std::vector<KVNode>  children;       // type == None 时有效

        const KVNode* Find(std::string_view key) const
        {
            for (const auto& child : children) {
                if (child.name.size() == key.size() &&
                    _strnicmp(child.name.c_str(), key.data(), key.size()) == 0)
                    return &child;
            }
            return nullptr;
        }
    };

    class KVReader
    {
    public:
        explicit KVReader(const std::vector<uint8_t>& data) : m_data(data) {}

        // 逐字段读取，任何越界/畸形输入都返回 false，绝不抛异常。
        bool ReadNode(KVNode& out, int depth)
        {
            if (depth > kMaxDepth) return false;

            while (true) {
                const uint8_t raw = ReadU8();
                if (!m_ok) return false;
                if (raw == static_cast<uint8_t>(KVType::End)) return true;
                if (raw > static_cast<uint8_t>(KVType::End)) return false;

                KVNode node;
                node.type = static_cast<KVType>(raw);
                if (!ReadString(node.name)) return false;

                switch (node.type) {
                case KVType::None:
                    if (!ReadNode(node.children.emplace_back(), depth + 1))
                        return false;
                    break;
                case KVType::String:
                    if (!ReadString(node.stringValue)) return false;
                    node.int32Value = atoi(node.stringValue.c_str());
                    break;
                case KVType::Int32:
                case KVType::WideString:
                case KVType::Color:
                case KVType::Pointer: {
                    const uint32_t v = ReadU32();
                    if (!m_ok) return false;
                    node.int32Value = static_cast<int32_t>(v);
                    break;
                }
                case KVType::Int64:
                case KVType::Float32:
                    if (!Skip(8)) return false;
                    break;
                default:
                    return false;
                }

                out.children.push_back(std::move(node));
            }
        }

    private:
        uint8_t ReadU8()
        {
            if (m_pos >= m_data.size()) { m_ok = false; return 0; }
            return m_data[m_pos++];
        }

        uint32_t ReadU32()
        {
            if (m_pos + 4 > m_data.size()) { m_ok = false; return 0; }
            uint32_t v;
            memcpy(&v, m_data.data() + m_pos, 4);
            m_pos += 4;
            return v;
        }

        bool ReadString(std::string& out)
        {
            out.clear();
            while (true) {
                if (m_pos >= m_data.size()) { m_ok = false; return false; }
                const char c = static_cast<char>(m_data[m_pos++]);
                if (c == '\0') return true;
                out.push_back(c);
                if (out.size() > kMaxNameBytes) { m_ok = false; return false; }
            }
        }

        bool Skip(size_t n)
        {
            if (m_pos + n > m_data.size()) { m_ok = false; return false; }
            m_pos += n;
            return true;
        }

        const std::vector<uint8_t>& m_data;
        size_t m_pos = 0;
        bool   m_ok  = true;
    };

    bool IsAchievementStat(const KVNode& stat)
    {
        // type 字符串：ACHIEVEMENTS / GROUPACHIEVEMENTS
        const KVNode* typeStr = stat.Find("type");
        if (typeStr && !typeStr->stringValue.empty()) {
            if (_stricmp(typeStr->stringValue.c_str(), "ACHIEVEMENTS") == 0 ||
                _stricmp(typeStr->stringValue.c_str(), "GROUPACHIEVEMENTS") == 0)
                return true;
        }
        // type_int：4 = Achievements、5 = GroupAchievements
        const KVNode* typeInt = stat.Find("type_int");
        if (typeInt && (typeInt->int32Value == 4 || typeInt->int32Value == 5))
            return true;
        return false;
    }

    // 数一个 stat 节点下的成就数（bits 子节点的条目数）
    uint32_t CountStatAchievements(const KVNode& stat)
    {
        if (!IsAchievementStat(stat)) return 0;
        const KVNode* bits = stat.Find("bits");
        if (!bits) return 0;
        return static_cast<uint32_t>(bits->children.size());
    }

    std::mutex g_cacheMutex;
    std::unordered_map<uint32_t, uint32_t> g_cache;   // appid -> 成就总数（只存解析成功的）
} // namespace

namespace StatsSchemaCache
{

bool GetAchievementTotal(AppId_t appId, uint32_t& total)
{
    if (appId == 0) return false;

    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        const auto it = g_cache.find(appId);
        if (it != g_cache.end()) {
            total = it->second;
            return true;
        }
    }

    if (SteamInstallPath[0] == '\0') return false;

    char path[512];
    snprintf(path, sizeof(path), "%s\\appcache\\stats\\UserGameStatsSchema_%u.bin",
             SteamInstallPath, static_cast<unsigned>(appId));

    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return false;
    if (std::filesystem::file_size(path, ec) > kMaxSchemaFileBytes || ec) return false;

    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    if (data.empty()) return false;

    KVReader reader(data);
    KVNode root;
    if (!reader.ReadNode(root, 0)) return false;

    // 根节点下找以 appid 命名的节点 → stats → 逐个 stat 累加成就数
    char appidName[16];
    snprintf(appidName, sizeof(appidName), "%u", static_cast<unsigned>(appId));

    const KVNode* appNode = root.Find(appidName);
    if (!appNode) return false;
    const KVNode* stats = appNode->Find("stats");
    if (!stats) return false;

    uint32_t count = 0;
    for (const auto& stat : stats->children)
        count += CountStatAchievements(stat);

    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        g_cache[appId] = count;
    }
    LOG_ACHIEVEMENT_DEBUG("StatsSchemaCache: appId={} achievement total = {}", appId, count);

    total = count;
    return true;
}

} // namespace StatsSchemaCache
