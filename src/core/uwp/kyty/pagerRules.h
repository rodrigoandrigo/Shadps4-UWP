#ifndef KYTY_XBOX_PAGER_RULES_H_
#define KYTY_XBOX_PAGER_RULES_H_

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Xbox::Pager {

inline constexpr uint64_t kMiB = uint64_t{1} << 20u;

class IdleTime {
public:
    static constexpr uint64_t kBaseMs = 8000;
    static constexpr uint64_t kLeastMs = 1000;
    static constexpr double kDoubling = 128.0;

    uint64_t Update(uint64_t now_ms, uint64_t quick_returns, bool above_budget) {
        const double elapsed = static_cast<double>(now_ms - m_last_ms);
        m_churn = m_churn * std::exp2(-elapsed / 10000.0) +
                  static_cast<double>(quick_returns - m_last_returns);
        m_last_ms = now_ms;
        m_last_returns = quick_returns;
        if (above_budget) {
            return kLeastMs;
        }
        return static_cast<uint64_t>(static_cast<double>(kBaseMs) *
                                     (1.0 + std::min(m_churn / kDoubling, 3.0)));
    }

private:
    double m_churn = 0.0;
    uint64_t m_last_ms = 0;
    uint64_t m_last_returns = 0;
};

} // namespace Xbox::Pager

#endif
