#include <bits/stdc++.h>

#include <ext/pb_ds/assoc_container.hpp>
#include <ext/pb_ds/tree_policy.hpp>
using namespace std;

// 使用 GCC 扩展（bits、PBDS、__int128 和 __builtin_*），请选 GNU++17 或更高。
#if __cplusplus < 201703L
#error "This template requires GNU C++17 or later."
#endif

// #include <boost/multiprecision/cpp_int.hpp>
// using Big = boost::multiprecision::uint256_t;
using i64 = long long;
using i128 = __int128_t;
using u64 = unsigned long long;
using u128 = __uint128_t;
using ll = long long;
using ull = unsigned long long;
using f32 = float;
using f64 = double;
using f80 = long double;

using pii = pair<int, int>;
using pll = pair<i64, i64>;

using vi = vector<int>;
using vvi = vector<vi>;
using vll = vector<i64>;
using vvll = vector<vll>;

// C++ 没有 min=/max= 运算符；返回 true 表示 value 确实被更新。
// 两个参数故意要求同类型，避免隐式窄化以及有符号/无符号混合比较。
template <typename T>
constexpr bool chmin(T& value, const T& candidate) {
    if (candidate < value) {
        value = candidate;
        return true;
    }
    return false;
}

template <typename T>
constexpr bool chmax(T& value, const T& candidate) {
    if (value < candidate) {
        value = candidate;
        return true;
    }
    return false;
}

// PBDS 顺序统计树（GCC 扩展，洛谷选择 GNU++17/20 时可用）
template <typename Key, typename Compare = std::less<Key>>
using ordered_set =
    __gnu_pbds::tree<Key, __gnu_pbds::null_type, Compare,
                     __gnu_pbds::rb_tree_tag,
                     __gnu_pbds::tree_order_statistics_node_update>;

// 支持重复值的顺序统计树。不要用 less_equal 伪造 multiset：
// less_equal 不是严格弱序，会破坏 PBDS 的查找、排名和删除。
template <typename T>
class ordered_multiset {
    using key_type = std::pair<T, u64>;
    using tree_type = ordered_set<key_type>;

   public:
    using size_type = typename tree_type::size_type;

    bool empty() const { return tree_.empty(); }
    size_type size() const { return tree_.size(); }

    void clear() {
        tree_.clear();
        next_id_ = 0;
    }

    void insert(const T& value) {
        assert(next_id_ != std::numeric_limits<u64>::max());
        tree_.insert({value, next_id_++});
    }

    // 删除任意一个等于 value 的元素；成功时返回 true。
    bool erase_one(const T& value) {
        auto it = tree_.lower_bound({value, 0});
        if (it == tree_.end() || it->first != value) return false;
        tree_.erase(it);
        return true;
    }

    size_type count_less(const T& value) const {
        return tree_.order_of_key({value, 0});
    }

    size_type count_less_equal(const T& value) const {
        return tree_.order_of_key({value, std::numeric_limits<u64>::max()});
    }

    size_type count(const T& value) const {
        return count_less_equal(value) - count_less(value);
    }

    // 返回第 k 小的值（k 从 0 开始）；越界时返回 nullopt。
    std::optional<T> kth(size_type k) const {
        auto it = tree_.find_by_order(k);
        if (it == tree_.end()) return std::nullopt;
        return it->first;
    }

   private:
    tree_type tree_;
    u64 next_id_ = 0;
};

// 参数必须是生命周期稳定、无副作用的容器左值
#define all(x) std::begin(x), std::end(x)

// 要求 x >= 0；lowbit(0) == 0；返回对应的无符号类型
template <typename T>
constexpr auto lowbit(T x) noexcept {
    static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>);

    if constexpr (std::is_signed_v<T>) {
        assert(x >= 0);
    }

    using U = std::make_unsigned_t<T>;
    const U u = static_cast<U>(x);
    return u & (U{0} - u);
}

// GCC 位运算内置函数的安全封装（支持不超过 64 位的整数）
namespace bitop {

template <typename T>
constexpr auto to_unsigned(T x) noexcept {
    using V = std::remove_cv_t<T>;
    static_assert(std::is_integral_v<V> && !std::is_same_v<V, bool>);

    using U = std::make_unsigned_t<V>;
    static_assert(std::numeric_limits<U>::digits <=
                  std::numeric_limits<unsigned long long>::digits);
    return static_cast<U>(x);
}

// 二进制中 1 的个数：__builtin_popcountll
template <typename T>
constexpr int popcount(T x) noexcept {
    const auto u = to_unsigned(x);
    return __builtin_popcountll(static_cast<unsigned long long>(u));
}

// 前导零个数：与 __builtin_clz/clzll 不同，x == 0 时返回类型位数
template <typename T>
constexpr int countl_zero(T x) noexcept {
    const auto u = to_unsigned(x);
    using U = decltype(u);
    constexpr int width = std::numeric_limits<U>::digits;
    constexpr int ull_width = std::numeric_limits<unsigned long long>::digits;

    if (u == 0) return width;
    return __builtin_clzll(static_cast<unsigned long long>(u)) -
           (ull_width - width);
}

// 后缀零个数：与 __builtin_ctz/ctzll 不同，x == 0 时返回类型位数
template <typename T>
constexpr int countr_zero(T x) noexcept {
    const auto u = to_unsigned(x);
    using U = decltype(u);
    constexpr int width = std::numeric_limits<U>::digits;

    if (u == 0) return width;
    return __builtin_ctzll(static_cast<unsigned long long>(u));
}

// 最低位 1 的位置（从 1 开始）；x == 0 时返回 0，等价于 __builtin_ffsll
template <typename T>
constexpr int first_one(T x) noexcept {
    const auto u = to_unsigned(x);
    return u == 0 ? 0 : countr_zero(u) + 1;
}

template <typename T>
constexpr int bit_width(T x) noexcept {
    const auto u = to_unsigned(x);
    using U = decltype(u);
    return std::numeric_limits<U>::digits - countl_zero(u);
}

// 最高/最低位 1 的下标（从 0 开始）；x == 0 时返回 -1
template <typename T>
constexpr int msb_index(T x) noexcept {
    return bit_width(x) - 1;
}

template <typename T>
constexpr int lsb_index(T x) noexcept {
    const auto u = to_unsigned(x);
    return u == 0 ? -1 : countr_zero(u);
}

template <typename T>
constexpr bool has_single_bit(T x) noexcept {
    return popcount(x) == 1;
}

template <typename T>
constexpr int parity(T x) noexcept {
    const auto u = to_unsigned(x);
    return __builtin_parityll(static_cast<unsigned long long>(u));
}

constexpr uint16_t bswap16(uint16_t x) noexcept { return __builtin_bswap16(x); }

constexpr uint32_t bswap32(uint32_t x) noexcept { return __builtin_bswap32(x); }

constexpr uint64_t bswap64(uint64_t x) noexcept { return __builtin_bswap64(x); }

}  // namespace bitop

// 返回 true 表示发生溢出；result 会被写入转换后的运算结果
namespace checked {

template <typename A, typename B, typename R>
constexpr bool add(A a, B b, R& result) noexcept {
    static_assert(std::is_integral_v<A> && std::is_integral_v<B> &&
                  std::is_integral_v<R> && !std::is_same_v<R, bool>);
    return __builtin_add_overflow(a, b, &result);
}

template <typename A, typename B, typename R>
constexpr bool sub(A a, B b, R& result) noexcept {
    static_assert(std::is_integral_v<A> && std::is_integral_v<B> &&
                  std::is_integral_v<R> && !std::is_same_v<R, bool>);
    return __builtin_sub_overflow(a, b, &result);
}

template <typename A, typename B, typename R>
constexpr bool mul(A a, B b, R& result) noexcept {
    static_assert(std::is_integral_v<A> && std::is_integral_v<B> &&
                  std::is_integral_v<R> && !std::is_same_v<R, bool>);
    return __builtin_mul_overflow(a, b, &result);
}

}  // namespace checked

// 位运算使用示例（可复制到 solve 中运行）：
// 位运算函数建议传入无符号整数；负数会被当作同宽度的无符号值处理。
//
// {
//     u64 x = 40;  // 二进制为 101000，第 5 位和第 3 位是 1
//
//     // 1. 统计二进制中 1 的数量
//     int ones = bitop::popcount(x);  // 2
//
//     // 2. 统计前导零和后缀零
//     // u64 通常为 64 位，所以 40 前面有 64 - 6 = 58 个零。
//     int leading_zeros = bitop::countl_zero(x);   // 58
//     int trailing_zeros = bitop::countr_zero(x); // 3
//     // 与原始 __builtin_clzll/__builtin_ctzll 不同，封装允许传 0：
//     int zero_leading = bitop::countl_zero(0ULL);   // 64
//     int zero_trailing = bitop::countr_zero(0ULL); // 64
//
//     // 3. 寻找最低位和最高位的 1
//     int first = bitop::first_one(x); // 4，从 1 开始，等价于 ffs 语义
//     int high = bitop::msb_index(x);  // 5，从 0 开始
//     int low = bitop::lsb_index(x);   // 3，从 0 开始
//     // 对于 x == 0，msb_index 和 lsb_index 都返回 -1。
//
//     // 4. 计算表示 x 所需的二进制位数
//     int len = bitop::bit_width(x); // 6，因为 40 的二进制是 101000
//     // x > 0 时：msb_index(x) == floor(log2(x))。
//     // bit_width(x) == floor(log2(x)) + 1，且不会有浮点精度问题。
//
//     // 5. 判断是否为 2 的幂，以及 1 的数量的奇偶性
//     bool power2 = bitop::has_single_bit(x); // false，40 有两个 1
//     int odd_ones = bitop::parity(x);        // 0，两个 1 是偶数个
//     bool eight_is_power2 =
//         bitop::has_single_bit(8ULL);         // true，8 == 2^3
//
//     // 6. lowbit 返回最低位 1 所代表的数值
//     u64 lowest_value = lowbit(x); // 8，因为最低位 1 在第 3 位
//     // 三个相关结果的区别：
//     // lowbit(40) == 8，lsb_index(40) == 3，first_one(40) == 4。
//
//     // 7. 不使用浮点数求不超过 x 的最大 2 的幂（要求 x > 0）
//     u64 highest_power = 1ULL << bitop::msb_index(x); // 32
//
//     // 8. 遍历 mask 中所有为 1 的位
//     u64 mask = x;
//     while (mask != 0) {
//         int bit = bitop::lsb_index(mask);
//         // 此处处理第 bit 个元素；x == 40 时依次得到 3、5。
//         mask &= mask - 1; // 删除当前最低位的 1
//     }
//
//     // 9. 反转字节顺序，注意这不是反转每一个二进制位
//     uint32_t swapped = bitop::bswap32(0x12345678U);
//     // swapped == 0x78563412U
// }
//
// 整数溢出检测示例：
//
// {
//     // 返回 true 表示发生溢出；没有溢出时才能使用 result。
//     // 是否溢出由结果变量的类型决定。
//     int result32 = 0;
//     bool overflow32 =
//         checked::add(INT_MAX, 1, result32); // true，超出 int 范围
//
//     i64 result64 = 0;
//     bool overflow64 =
//         checked::add(INT_MAX, 1, result64); // false
//     // result64 == 2147483648LL
//
//     i64 product = 0;
//     bool mul_overflow = checked::mul(
//         4'000'000'000LL, 4'000'000'000LL, product); // true
//
//     i64 difference = 0;
//     bool sub_overflow = checked::sub(
//         numeric_limits<i64>::min(), 1LL, difference); // true
//
//     if (!mul_overflow) {
//         // 只有没有溢出时，product 才是可用的精确结果。
//         cout << product << '\n';
//     }
// }

// 本地调试输出
///----------

#ifdef LOCAL

namespace dbg_detail {

template <typename T, typename = void>
struct is_streamable : std::false_type {};

template <typename T>
struct is_streamable<T, std::void_t<decltype(std::declval<std::ostream&>()
                                             << std::declval<const T&>())>>
    : std::true_type {};

template <typename T, typename = void>
struct is_range : std::false_type {};

template <typename T>
struct is_range<T, std::void_t<decltype(std::begin(std::declval<const T&>())),
                               decltype(std::end(std::declval<const T&>()))>>
    : std::true_type {};

template <typename T>
struct is_pair : std::false_type {};

template <typename A, typename B>
struct is_pair<std::pair<A, B>> : std::true_type {};

template <typename T>
void print_one(std::ostream& os, const T& value) {
    using Raw = std::remove_reference_t<T>;
    using U = std::decay_t<T>;

    constexpr bool non_char_array =
        std::is_array_v<Raw> &&
        !std::is_same_v<std::remove_cv_t<std::remove_extent_t<Raw>>, char>;

    if constexpr (is_pair<U>::value) {
        os << '(';
        print_one(os, value.first);
        os << ", ";
        print_one(os, value.second);
        os << ')';
    } else if constexpr (non_char_array ||
                         (is_range<Raw>::value && !is_streamable<U>::value)) {
        os << '[';
        bool first = true;

        for (const auto& item : value) {
            if (!first) os << ", ";
            first = false;
            print_one(os, item);
        }

        os << ']';
    } else if constexpr (is_streamable<U>::value) {
        os << value;
    } else {
        os << "<unprintable>";
    }
}

inline void print_many() { std::cerr << '\n'; }

template <typename First, typename... Rest>
void print_many(const First& first, const Rest&... rest) {
    print_one(std::cerr, first);
    ((std::cerr << ", ", print_one(std::cerr, rest)), ...);
    std::cerr << '\n';
}

}  // namespace dbg_detail

#define dbg(...)                                    \
    do {                                            \
        std::cerr << "[" << #__VA_ARGS__ << "] = "; \
        ::dbg_detail::print_many(__VA_ARGS__);      \
    } while (false)

#else

#define dbg(...) ((void)0)

#endif

///----------

template <typename T>
vector<T> make_v(size_t n, T value) {
    return vector<T>(n, value);
}

template <typename... Args>
auto make_v(size_t n, Args... args) {
    auto inner = make_v(args...);
    return vector<decltype(inner)>(n, inner);
}

// dbg 使用示例（仅在定义 LOCAL 时向 cerr 输出，提交时不定义 LOCAL）：
// int x = 7;
// vi a{1, 2, 3};
// pii p{4, 5};
// int raw[]{6, 7, 8};
// dbg(x);          // [x] = 7
// dbg(a, p);       // [a, p] = [1, 2, 3], (4, 5)
// dbg(raw);        // [raw] = [6, 7, 8]

// make_v 使用示例（最后一个参数决定最内层元素的类型和初值）：
// auto a1 = make_v(n, -1);                 // vector<int>(n, -1)
// auto dp = make_v(n + 1, m + 1, 0LL);     // 二维 vector<i64>，初值为 0
// auto dis = make_v(n, m, INF64);           // 二维 vector<i64>，初值为 INF64
// auto vis = make_v(n, m, false);           // 二维 vector<bool>，初值为 false
// auto memo = make_v(n, m, k, -1LL);        // 三维 vector<i64>，初值为 -1

// chmin/chmax 使用示例：
// i64 best = INF64;
// bool changed = chmin(best, 100LL);  // best = 100，changed = true
// chmin(best, 120LL);                 // best 不变；可以忽略返回值
// chmax(best, 200LL);                 // best = 200
// 注意参数类型必须一致，例如 i64 变量搭配 3LL 或 i64{3}。

// PBDS 使用示例：
// ordered_set<int> os;
// os.insert(10);
// os.insert(20);
// os.insert(30);
// auto less_than_20 = os.order_of_key(20);  // 1：严格小于 20 的数量
// auto it = os.find_by_order(1);            // 下标从 0 开始，指向第 2 小
// if (it != os.end()) cout << *it << '\n';  // 20；越界时 it == end()
// os.erase(20);
//
// ordered_multiset<int> ms;
// ms.insert(5);
// ms.insert(5);
// ms.insert(8);
// ms.count_less(5);        // 0
// ms.count_less_equal(5);  // 2
// ms.count(5);             // 2
// ms.kth(1);               // optional<int>{5}，第 2 小
// ms.erase_one(5);         // 删除一个 5，返回 true
// // x 的 1-based 排名：ms.count_less(x) + 1
// // 前驱的 0-based 位置：ms.count_less(x) - 1
// // 后继的 0-based 位置：ms.count_less_equal(x)

// 64 位安全模乘；要求 mod != 0。
constexpr u64 mul_mod(u64 a, u64 b, u64 mod) noexcept {
    assert(mod != 0);
    return static_cast<u64>(static_cast<u128>(a) * b % mod);
}

// 快速幂计算 (a^b) % mod；要求 mod != 0。
// 参数使用 u64，乘法使用 u128 中间值，支持完整 u64 范围。
constexpr u64 quick_power(u64 a, u64 b, u64 mod) noexcept {
    assert(mod != 0);
    u64 res = 1 % mod;
    a %= mod;
    while (b > 0) {
        if (b & 1) res = mul_mod(res, a, mod);
        a = mul_mod(a, a, mod);
        b >>= 1;
    }
    return res;
}

// 确定性 Miller-Rabin 素性测试，适用于完整 u64 范围。
// 复杂度 O(7 log n)，基底集合不能随意删减。
bool miller_rabin(u64 n) noexcept {
    if (n < 2) return false;
    static constexpr u64 small_primes[] = {2,  3,  5,  7,  11, 13,
                                           17, 19, 23, 29, 31, 37};
    for (u64 p : small_primes) {
        if (n % p == 0) return n == p;
    }

    u64 d = n - 1;
    int s = 0;
    while ((d & 1) == 0) {
        d >>= 1;
        ++s;
    }

    static constexpr u64 bases[] = {2,       325,       9'375,        28'178,
                                    450'775, 9'780'504, 1'795'265'022};
    for (u64 a : bases) {
        if (a % n == 0) continue;
        u64 x = quick_power(a, d, n);
        if (x == 1 || x == n - 1) continue;

        bool reached_minus_one = false;
        for (int r = 1; r < s; r++) {
            x = mul_mod(x, x, n);
            if (x == n - 1) {
                reached_minus_one = true;
                break;
            }
        }
        if (!reached_minus_one) return false;
    }
    return true;
}

// 质数模数下的阶乘、逆阶乘、排列数和组合数。
// ensure(n) 按需扩容到 n；扩容总复杂度 O(新增长度)，已预处理查询 O(1)。
// P/C 的阶乘表公式要求 0 <= n < modulus；n >= modulus 时应按题使用
// Lucas 定理或其他算法，不能继续套用阶乘与逆阶乘公式。
// 构造时会验证模数；不满足条件会抛出 invalid_argument/out_of_range。
// 两张 u64 表约占用 16 * (prepared() + 1) 字节。
class PrimeComb {
   public:
    explicit PrimeComb(u64 modulus, u64 initial_n = 0)
        : modulus_(modulus), factorial_{1}, inverse_factorial_{1} {
        if (!miller_rabin(modulus_)) {
            throw std::invalid_argument("PrimeComb: modulus must be prime");
        }
        ensure(initial_n);
    }

    u64 modulus() const noexcept { return modulus_; }

    u64 prepared() const noexcept {
        return static_cast<u64>(factorial_.size() - 1);
    }

    void ensure(u64 n) {
        require_table_index(n);
        const size_t target = checked_index(n);
        if (target < factorial_.size()) return;

        if (target >= factorial_.max_size() ||
            target >= inverse_factorial_.max_size()) {
            throw std::length_error("PrimeComb: table is too large");
        }

        const size_t old_size = factorial_.size();
        factorial_.resize(target + 1);
        inverse_factorial_.resize(target + 1);

        for (size_t i = old_size; i <= target; ++i) {
            const u64 x = static_cast<u64>(i);
            u64 inverse_x = 1;

            if (x > 1) {
                // modulus = q*x + r，可推出 inv(x) = -q*inv(r)。
                const u64 remainder = modulus_ % x;
                assert(remainder != 0);

                // inv(r) = (r-1)! / r!
                const size_t r = static_cast<size_t>(remainder);
                const u64 inverse_r =
                    mul_mod(factorial_[r - 1], inverse_factorial_[r], modulus_);
                const u64 term = mul_mod(modulus_ / x, inverse_r, modulus_);
                assert(term != 0);
                inverse_x = modulus_ - term;
            }

            factorial_[i] = mul_mod(factorial_[i - 1], x, modulus_);
            inverse_factorial_[i] =
                mul_mod(inverse_factorial_[i - 1], inverse_x, modulus_);
        }
    }

    // 返回 n! mod modulus。n >= modulus 时结果必为 0。
    u64 factorial(u64 n) {
        if (n >= modulus_) return 0;
        ensure(n);
        return factorial_[static_cast<size_t>(n)];
    }

    // 返回 (n!)^(-1) mod modulus；仅在 n < modulus 时存在。
    u64 inverse_factorial(u64 n) {
        ensure(n);
        return inverse_factorial_[static_cast<size_t>(n)];
    }

    // P(n, k) = n! / (n-k)!
    u64 P(u64 n, u64 k) {
        if (k > n) return 0;
        ensure(n);

        const size_t nn = static_cast<size_t>(n);
        const size_t nk = static_cast<size_t>(n - k);
        return mul_mod(factorial_[nn], inverse_factorial_[nk], modulus_);
    }

    // C(n, k) = n! / (k!(n-k)!)
    u64 C(u64 n, u64 k) {
        if (k > n) return 0;
        ensure(n);

        const size_t nn = static_cast<size_t>(n);
        const size_t kk = static_cast<size_t>(k);
        const size_t nk = static_cast<size_t>(n - k);
        const u64 denominator =
            mul_mod(inverse_factorial_[kk], inverse_factorial_[nk], modulus_);
        return mul_mod(factorial_[nn], denominator, modulus_);
    }

   private:
    void require_table_index(u64 n) const {
        if (n >= modulus_) {
            throw std::out_of_range(
                "PrimeComb: P/C tables require n < modulus; consider "
                "Lucas theorem");
        }
    }

    static size_t checked_index(u64 n) {
        if (static_cast<u128>(n) + 1 >
            static_cast<u128>(std::numeric_limits<size_t>::max())) {
            throw std::length_error("PrimeComb: table is too large");
        }
        return static_cast<size_t>(n);
    }

    u64 modulus_;
    vector<u64> factorial_;
    vector<u64> inverse_factorial_;
};

// PrimeComb 使用示例（结果均对 modulus 取模）：
// PrimeComb comb(1'000'000'007ULL, 100'000); // 可在构造时预处理
// comb.factorial(10);         // 3628800，即 10!
// comb.inverse_factorial(10); // 10! 的乘法逆元
// comb.P(10, 3);              // 720，即 10*9*8
// comb.C(10, 3);              // 120
// comb.C(10, 11);             // 0，k > n
// comb.ensure(200'000);       // 之后可线性增量扩容，不会缩小已有表

// 整数 INF 是为加法预留余量的算法哨兵，并不等于类型的真正最大值。
// 若合法答案可能超过它，或要从 INF 继续运算，应按题目约束另行处理。
constexpr int INF32 = std::numeric_limits<int>::max() / 4;
constexpr i64 INF64 = std::numeric_limits<i64>::max() / 4;
constexpr int NINF32 = -INF32;
constexpr i64 NINF64 = -INF64;

// 浮点数的 max() 是最大有限正数，lowest() 是最负的有限数，
// infinity() 才是正无穷。注意 min() 是最小正正规数，不是最负值。
constexpr f32 F32_MAX = std::numeric_limits<f32>::max();
constexpr f32 F32_LOWEST = std::numeric_limits<f32>::lowest();
constexpr f32 INF_F32 = std::numeric_limits<f32>::infinity();
constexpr f64 F64_MAX = std::numeric_limits<f64>::max();
constexpr f64 F64_LOWEST = std::numeric_limits<f64>::lowest();
constexpr f64 INF_F64 = std::numeric_limits<f64>::infinity();
constexpr f80 F80_MAX = std::numeric_limits<f80>::max();
constexpr f80 F80_LOWEST = std::numeric_limits<f80>::lowest();
constexpr f80 INF_F80 = std::numeric_limits<f80>::infinity();

// 浮点误差阈值 EPS 与数值尺度、运算次数有关，应该按题目单独定义。
constexpr ll N = 100'000 + 10;
constexpr i64 MOD = 1'000'000'007LL;
constexpr ll MAXN = 100'000 + 5;

void solve() {}

signed main() {
    ios_base::sync_with_stdio(false);
    cin.tie(nullptr);
    ll T = 1LL;
    // cin >> T;
    while (T--) solve();
    return 0;
}
