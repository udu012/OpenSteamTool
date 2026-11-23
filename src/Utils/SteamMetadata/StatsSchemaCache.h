#ifndef STATSSCHEMACACHE_H
#define STATSSCHEMACACHE_H

#include "Steam/Types.h"

#include <cstdint>

namespace StatsSchemaCache
{
    // 统计 app 的成就总数（schema 里的 achievement bits 数）。
    // 数据源：<Steam>/appcache/stats/UserGameStatsSchema_<appid>.bin，
    // 这是 Steam 客户端自己缓存的成就 schema，游戏被加载过后就会存在。
    //
    // 返回 true 表示拿到了确定的成就总数（含 0：该游戏没有成就）；
    // 返回 false 表示本地没有 schema 或解析失败，调用方应跳过注入。
    //
    // 只缓存解析成功的结果；文件不存在不缓存——schema 可能在游戏
    // 首次运行后才落盘，下次查询要重试文件。
    bool GetAchievementTotal(AppId_t appId, uint32_t& total);
}

#endif // STATSSCHEMACACHE_H
