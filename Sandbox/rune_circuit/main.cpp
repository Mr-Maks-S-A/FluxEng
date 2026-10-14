/**
 * @file main.cpp
 * @brief RuneCircuit — язык рунных схем: заклинание как транзистор (по презентации «Грамматика рунного круга»).
 *
 * Идея презентации:
 *   - **Питание** — мана среды: её много, она сырая и разлита по миру; она и питает эффект.
 *   - **Сигнал на затворе** — личная мана: её мало, она не питает эффект, а открывает и направляет поток.
 *   - **Цепь** — рунная схема: решает, как поток преобразуется, куда идёт и когда срабатывает.
 *   - Усиление = мана среды / личная мана; растёт с навыком, качеством инструментов и чистотой схемы.
 *   - **Аспекты**: поток окрашен (нейтральный, огонь, земля, воздух, вода). Цена смены аспекта зависит от родства
 *     (тот же — почти даром, соседний — дёшево, из нейтрального — средне, противоположный — дорого).
 *     Остаток заклинания окрашивает среду: повтор того же аспекта дешевле, противоположный мешает.
 *     Окраска выцветает (релаксация), а избыток аспекта меняет мир (насыщение).
 *   - Перегрузка — не класс рун, а риск: ошибка в схеме — срыв и взрыв.
 *
 * Здесь это — **текстовый язык** (`.circle`): разбор с диагнозом по строкам, проверка схемы (разрыв дуги, петли, несовместимые
 * связи), вложенные круги (вызов функции с параметрами) и тактовая симуляция потока маны. Консольный, без окна и GPU,
 * зависит только от стандартной библиотеки: язык можно прочесть и проверить отдельно от движка. Подробно — LANGUAGE.md.
 *
 * Использование:
 *   RuneCircuit [файл.circle ...]     исполнить схемы по очереди в одном мире (по умолчанию — все из каталога circles)
 *               [--casts N]           сколько раз подряд кастовать каждую схему (по умолчанию 2: видно «повтор дешевле»)
 *               [--gap T]             тактов тишины между кастами (выцветание окраски; по умолчанию 0)
 *               [--trace]             по тактам: сигналы, поток, личная мана
 *   RuneCircuit --check               самопроверка языка; последняя строка `OK`
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef RUNE_CIRCUIT_DIR
#define RUNE_CIRCUIT_DIR "circles"
#endif

namespace {

// ------------------------------------------------------------------------------------------------ аспекты и миксы

enum Aspect { Neutral, Fire, Earth, Air, Water, AspectCount };
constexpr const char* aspect_names[AspectCount] = {"нейтральная", "огонь", "земля", "воздух", "вода"};
constexpr const char* aspect_keys[AspectCount] = {"neutral", "fire", "earth", "air", "water"};

/// Количество маны каждого аспекта.
using Mix = std::array<double, AspectCount>;

double total(const Mix& m) {
    double s = 0;
    for (double v : m) s += v;
    return s;
}
Mix operator*(Mix m, double k) {
    for (double& v : m) v *= k;
    return m;
}
Mix operator-(Mix a, const Mix& b) {
    for (int i = 0; i < AspectCount; ++i) a[i] -= b[i];
    return a;
}

Mix& operator+=(Mix& a, const Mix& b) {
    for (int i = 0; i < AspectCount; ++i) a[i] += b[i];
    return a;
}

std::optional<Aspect> aspect_by_key(const std::string& k) {
    for (int i = 0; i < AspectCount; ++i)
        if (k == aspect_keys[i]) return static_cast<Aspect>(i);
    return std::nullopt;
}

/// Доля потерь при смене аспекта `from → to` (она возвращается среде окрашенной в `to`).
/// Круг родства: огонь — земля — вода — воздух — огонь; противоположные: огонь/вода и земля/воздух.
double conversion_loss(Aspect from, Aspect to) {
    if (from == to) return 0.02;              // тот же аспект: почти даром
    if (from == Neutral) return 0.25;         // нейтральная → любой: средне
    if (to == Neutral) return 0.25;
    constexpr int ring[AspectCount] = {-1, 0, 1, 3, 2};  // огонь 0, земля 1, вода 2, воздух 3
    const int d = (ring[from] - ring[to] + 4) % 4;
    return d == 2 ? 0.60 : 0.10;              // противоположный: дорого; соседний: дёшево
}

// ------------------------------------------------------------------------------------------------ классы рун

enum class Domain { Flow, Signal };

struct ClassInfo {
    const char* name;
    Domain domain;
    const char* keys;      ///< допустимые параметры, через пробел
    const char* positional;///< параметр для записи без «ключ=» (может быть пустым)
};

constexpr ClassInfo classes[] = {
    // Источники и накопители, усилители, преобразователи (золотые руны — работают с потоком).
    {"Condenser", Domain::Flow, "power limit", "power"},
    {"Capacitor", Domain::Flow, "cap rate limit", "cap"},
    {"Amplifier", Domain::Flow, "ratio limit", "ratio"},
    {"Convert", Domain::Flow, "to share limit", "to"},
    // Эффекторы (белые руны — выводят эффект в мир): use — какую долю потока забрать, остальное идёт дальше.
    {"Matter", Domain::Flow, "use radius limit", "use"},
    {"Heat", Domain::Flow, "use limit", "use"},
    {"Impulse", Domain::Flow, "use limit", "use"},
    {"Shape", Domain::Flow, "use radius limit", "use"},
    // Порты вложенного круга и простое соединение.
    {"In", Domain::Flow, "", ""},
    {"Out", Domain::Flow, "", ""},
    {"Pass", Domain::Flow, "limit", ""},
    // Сенсоры и логика (синие руны — работают с сигналом).
    {"Touch", Domain::Signal, "", ""},
    {"Target", Domain::Signal, "", ""},
    {"Timer", Domain::Signal, "after", "after"},
    {"Distance", Domain::Signal, "lt", "lt"},
    {"And", Domain::Signal, "", ""},
    {"Or", Domain::Signal, "", ""},
    {"Not", Domain::Signal, "", ""},
    {"Delay", Domain::Signal, "ticks", "ticks"},
    {"Counter", Domain::Signal, "count", "count"},
};

const ClassInfo* find_class(const std::string& name) {
    for (const ClassInfo& c : classes)
        if (name == c.name) return &c;
    return nullptr;
}

// ------------------------------------------------------------------------------------------------ разбор текста

struct Diag {
    int line;
    std::string text;
};

using Props = std::map<std::string, std::string>;

struct Stmt {
    enum Kind { Rune, Flow, Signal, Call } kind = Rune;
    int line = 0;
    std::string id, cls;               ///< Rune: id и класс; Call: id и имя вложенного круга
    Props props;
    std::vector<std::string> chain;    ///< Flow/Signal: цепочка a -> b -> c
};

struct Def {
    std::string name;
    Props params;                      ///< параметры и значения по умолчанию
    std::vector<Stmt> body;
    int line = 0;
};

struct SpellHeader {
    double ring = 5;                   ///< радиус кольца, м
    int time = 10;                     ///< длительность, тактов
};

struct Source {
    std::map<std::string, Def> defs;
    Def spell;
    bool has_spell = false;
    SpellHeader header;
    Props caster, world;
    std::vector<Diag> diags;
};

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    for (std::string t; is >> t;) out.push_back(t);
    return out;
}

bool parse_number(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end && *end == 0 && std::isfinite(out);
}

void parse_props(const std::vector<std::string>& toks, std::size_t from, Props& out, Source& src, int line, const ClassInfo* ci) {
    for (std::size_t i = from; i < toks.size(); ++i) {
        const std::string& t = toks[i];
        const auto eq = t.find('=');
        if (eq == std::string::npos) {
            if (ci && ci->positional[0] && i == from) out[ci->positional] = t;
            else src.diags.push_back({line, "ожидалось «ключ=значение», получено «" + t + "»"});
        } else {
            out[t.substr(0, eq)] = t.substr(eq + 1);
        }
    }
}

Source parse(const std::string& text) {
    Source src;
    Def* cur = nullptr;
    Def def_tmp;
    bool in_def = false;
    int ln = 0;
    std::istringstream is(text);
    for (std::string raw; std::getline(is, raw);) {
        ++ln;
        if (const auto h = raw.find('#'); h != std::string::npos) raw.resize(h);
        std::vector<std::string> t = split_ws(raw);
        // «a -> b» без пробелов вокруг стрелок тоже допустим: разделим их.
        if (t.empty()) continue;
        const std::string& head = t[0];

        if (head == "spell" || head == "def") {
            if (cur) { src.diags.push_back({ln, "круг «" + cur->name + "» не закрыт словом end"}); }
            if (t.size() < 2) { src.diags.push_back({ln, "после «" + head + "» нужно имя"}); continue; }
            def_tmp = Def{};
            def_tmp.name = t[1];
            def_tmp.line = ln;
            in_def = head == "def";
            Props p;
            parse_props(t, 2, p, src, ln, nullptr);
            if (in_def) {
                def_tmp.params = p;
                if (src.defs.count(def_tmp.name)) src.diags.push_back({ln, "круг «" + def_tmp.name + "» уже определён"});
            } else {
                if (src.has_spell) src.diags.push_back({ln, "в файле может быть только одно заклинание (spell)"});
                for (auto& [k, v] : p) {
                    double x = 0;
                    if (!parse_number(v, x)) { src.diags.push_back({ln, "параметр " + k + " — не число: «" + v + "»"}); continue; }
                    if (k == "ring") src.header.ring = x;
                    else if (k == "time") src.header.time = static_cast<int>(x);
                    else src.diags.push_back({ln, "у spell нет параметра «" + k + "» (есть ring, time)"});
                }
            }
            src.spell.body.clear();
            cur = &def_tmp;
            continue;
        }
        if (head == "end") {
            if (!cur) { src.diags.push_back({ln, "end без spell/def"}); continue; }
            if (in_def) src.defs[cur->name] = *cur;
            else { src.spell = *cur; src.has_spell = true; }
            cur = nullptr;
            continue;
        }
        if (head == "caster" || head == "world") {
            Props p;
            parse_props(t, 1, p, src, ln, nullptr);
            (head == "caster" ? src.caster : src.world) = p;
            continue;
        }
        if (!cur) {
            // Файл без явного spell/end: заклинание — всё до конца файла.
            if (!src.has_spell && def_tmp.name.empty()) {
                def_tmp = Def{};
                def_tmp.name = "Spell";
                def_tmp.line = ln;
                cur = &def_tmp;
                in_def = false;
            } else {
                src.diags.push_back({ln, "руна вне круга"});
                continue;
            }
        }

        Stmt s;
        s.line = ln;
        if (head == "rune") {
            // rune id = Class [args]
            if (t.size() < 4 || t[2] != "=") { src.diags.push_back({ln, "ожидалось «rune имя = Класс [параметры]»"}); continue; }
            s.kind = Stmt::Rune;
            s.id = t[1];
            s.cls = t[3];
            const ClassInfo* ci = find_class(s.cls);
            if (!ci) { src.diags.push_back({ln, "неизвестный класс руны «" + s.cls + "»"}); continue; }
            parse_props(t, 4, s.props, src, ln, ci);
        } else if (head == "call") {
            if (t.size() < 4 || t[2] != "=") { src.diags.push_back({ln, "ожидалось «call имя = Круг [параметры]»"}); continue; }
            s.kind = Stmt::Call;
            s.id = t[1];
            s.cls = t[3];
            parse_props(t, 4, s.props, src, ln, nullptr);
        } else if (head == "flow" || head == "signal") {
            s.kind = head == "flow" ? Stmt::Flow : Stmt::Signal;
            const std::string arrow = head == "flow" ? "->" : "~>";
            bool ok = t.size() >= 4 && t.size() % 2 == 0;
            for (std::size_t i = 1; ok && i < t.size(); ++i) {
                if (i % 2 == 0) ok = t[i] == arrow;
                else s.chain.push_back(t[i]);
            }
            if (!ok) { src.diags.push_back({ln, "ожидалось «" + head + " a " + arrow + " b [" + arrow + " c ...]»"}); continue; }
        } else {
            src.diags.push_back({ln, "непонятная строка, начинается с «" + head + "»"});
            continue;
        }
        cur->body.push_back(s);
    }
    if (cur) {
        if (in_def || cur->name != "Spell" || true) {
            if (in_def) src.diags.push_back({ln, "круг «" + cur->name + "» не закрыт словом end"});
            else { src.spell = *cur; src.has_spell = true; }
        }
    }
    if (!src.has_spell && src.diags.empty()) src.diags.push_back({0, "в файле нет заклинания (spell)"});
    return src;
}

// ------------------------------------------------------------------------------------------------ схема после раскрытия

struct Node {
    std::string name;                  ///< полное имя (вложенные: «b.f»)
    std::string cls;
    const ClassInfo* info = nullptr;
    int line = 0;
    Props props;
    double num(const std::string& k, double def) const {
        const auto it = props.find(k);
        double v = def;
        if (it != props.end()) {
            if (const auto a = aspect_by_key(it->second); a && k == "to") return *a;
            parse_number(it->second, v);
        }
        return v;
    }
};

struct Circuit {
    std::vector<Node> nodes;
    std::vector<std::pair<int, int>> flow, sig;
    SpellHeader header;
    double skill = 1, tools = 1, purity = 1, personal = 100;
    Mix world{};
    double relax = 0.08, saturation = 30;
    double touch_at = 1, distance = 8;
    bool target_seen = true;
    std::string name;
    bool reset = false;   ///< world reset=1: схема начинает с чистого мира, а не с остатка прошлых заклинаний
};

struct Compiled {
    Circuit circuit;
    std::vector<Diag> diags;
    bool ok() const { return diags.empty(); }
};

constexpr int max_runes = 32;
constexpr double default_limit = 30;

struct Scope {
    std::map<std::string, std::pair<int, int>> names;   ///< id → (узел для входящих связей, узел для исходящих)
};

class Builder {
public:
    Builder(const Source& src, Circuit& c, std::vector<Diag>& d) : src_(src), c_(c), d_(d) {}

    void instantiate(const Def& def, const std::string& prefix, const Props& args, int depth) {
        if (depth > 4) { err(def.line, "вложенность кругов глубже 4 (рекурсия?) в «" + def.name + "»"); return; }
        Props bind = def.params;
        for (auto& [k, v] : args) {
            if (!def.params.count(k) && depth > 0) err(def.line, "у круга «" + def.name + "» нет параметра «" + k + "»");
            bind[k] = v;
        }
        Scope sc;
        // Проход 1: руны и вызовы.
        for (const Stmt& s : def.body) {
            if (s.kind == Stmt::Rune) {
                if (sc.names.count(s.id)) { err(s.line, "имя «" + s.id + "» уже занято"); continue; }
                Node n;
                n.name = prefix + s.id;
                n.cls = s.cls;
                n.info = find_class(s.cls);
                n.line = s.line;
                for (auto& [k, v] : s.props) n.props[k] = subst(v, bind, s.line);
                for (auto& [k, v] : n.props)
                    if (!std::strstr(n.info->keys, k.c_str()) || k.empty()) err(s.line, "у руны " + s.cls + " нет параметра «" + k + "»");
                const int idx = static_cast<int>(c_.nodes.size());
                c_.nodes.push_back(n);
                sc.names[s.id] = {idx, idx};
                if (s.cls == "In") in_[prefix] = idx;
                if (s.cls == "Out") out_[prefix] = idx;
            } else if (s.kind == Stmt::Call) {
                if (sc.names.count(s.id)) { err(s.line, "имя «" + s.id + "» уже занято"); continue; }
                const auto it = src_.defs.find(s.cls);
                if (it == src_.defs.end()) { err(s.line, "нет круга «" + s.cls + "» (def)"); continue; }
                Props a;
                for (auto& [k, v] : s.props) a[k] = subst(v, bind, s.line);
                const std::string child = prefix + s.id + ".";
                in_.erase(child);
                out_.erase(child);
                instantiate(it->second, child, a, depth + 1);
                if (!in_.count(child) || !out_.count(child)) { err(s.line, "в круге «" + s.cls + "» нужны руны In и Out, чтобы его вызывать"); continue; }
                sc.names[s.id] = {in_[child], out_[child]};
            }
        }
        // Проход 2: связи.
        for (const Stmt& s : def.body) {
            if (s.kind != Stmt::Flow && s.kind != Stmt::Signal) continue;
            for (std::size_t i = 0; i + 1 < s.chain.size(); ++i) {
                const auto a = sc.names.find(s.chain[i]);
                const auto b = sc.names.find(s.chain[i + 1]);
                if (a == sc.names.end()) { err(s.line, "нет руны «" + s.chain[i] + "»"); continue; }
                if (b == sc.names.end()) { err(s.line, "нет руны «" + s.chain[i + 1] + "»"); continue; }
                (s.kind == Stmt::Flow ? c_.flow : c_.sig).emplace_back(a->second.second, b->second.first);
                lines_[{a->second.second, b->second.first}] = s.line;
            }
        }
    }

    int line_of(int a, int b) const {
        const auto it = lines_.find({a, b});
        return it == lines_.end() ? 0 : it->second;
    }

private:
    void err(int line, std::string t) { d_.push_back({line, std::move(t)}); }
    std::string subst(const std::string& v, const Props& bind, int line) {
        if (v.empty() || v[0] != '$') return v;
        const auto it = bind.find(v.substr(1));
        if (it == bind.end()) { err(line, "нет параметра круга «" + v.substr(1) + "»"); return "0"; }
        return it->second;
    }

    const Source& src_;
    Circuit& c_;
    std::vector<Diag>& d_;
    std::map<std::string, int> in_, out_;
    std::map<std::pair<int, int>, int> lines_;
};

Compiled compile(const std::string& text) {
    Compiled r;
    Source src = parse(text);
    r.diags = src.diags;
    if (!r.ok()) return r;
    Circuit& c = r.circuit;
    c.name = src.spell.name;
    c.header = src.header;

    auto getnum = [&](const Props& p, const char* k, double& out) {
        const auto it = p.find(k);
        if (it == p.end()) return;
        if (!parse_number(it->second, out)) r.diags.push_back({0, std::string("параметр ") + k + " — не число: «" + it->second + "»"});
    };
    getnum(src.caster, "skill", c.skill);
    getnum(src.caster, "tools", c.tools);
    getnum(src.caster, "purity", c.purity);
    getnum(src.caster, "personal", c.personal);
    getnum(src.world, "mana", c.world[Neutral]);
    for (int a = Fire; a < AspectCount; ++a) getnum(src.world, aspect_keys[a], c.world[a]);
    getnum(src.world, "relax", c.relax);
    getnum(src.world, "saturation", c.saturation);
    getnum(src.world, "touch_at", c.touch_at);
    getnum(src.world, "distance", c.distance);
    c.reset = src.world.count("reset") && src.world.at("reset") != "0";
    if (src.world.count("target")) c.target_seen = src.world.at("target") != "0";
    if (c.skill <= 0 || c.tools <= 0 || c.purity <= 0) r.diags.push_back({0, "skill, tools и purity должны быть положительными"});
    if (c.header.time < 1 || c.header.time > 1000) r.diags.push_back({src.spell.line, "time должно быть от 1 до 1000 тактов"});

    Builder b(src, c, r.diags);
    b.instantiate(src.spell, "", {}, 0);
    if (!r.ok()) return r;

    // Проверка схемы.
    const int n = static_cast<int>(c.nodes.size());
    if (n > max_runes) r.diags.push_back({0, "в кольце " + std::to_string(n) + " рун, помещается не больше " + std::to_string(max_runes)});
    std::vector<int> fin(n), fout(n), sin(n);
    for (auto [a, bb] : c.flow) {
        const Node &na = c.nodes[a], &nb = c.nodes[bb];
        if (na.info->domain != Domain::Flow || nb.info->domain != Domain::Flow)
            r.diags.push_back({b.line_of(a, bb), "поток (flow) связывает только руны потока, а «" + (na.info->domain != Domain::Flow ? na.name : nb.name) + "» работает с сигналом — используйте signal"});
        ++fout[a];
        ++fin[bb];
    }
    for (auto [a, bb] : c.sig) {
        const Node &na = c.nodes[a], &nb = c.nodes[bb];
        if (na.info->domain != Domain::Signal)
            r.diags.push_back({b.line_of(a, bb), "сигнал (signal) идёт только от сенсора или логики, а «" + na.name + "» — руна потока"});
        if (nb.cls == "In" || nb.cls == "Out" || nb.cls == "Pass" || nb.cls == "Convert" || nb.info->domain == Domain::Flow)
            if (nb.cls != "Condenser" && nb.cls != "Amplifier" && nb.cls != "Capacitor" && nb.cls != "Convert" && nb.cls != "Matter" && nb.cls != "Heat" && nb.cls != "Impulse" && nb.cls != "Shape")
                r.diags.push_back({b.line_of(a, bb), "сигнал нельзя подать на «" + nb.name + "»"});
        ++sin[bb];
    }
    // Числа параметров.
    for (const Node& nd : c.nodes) {
        for (auto& [k, v] : nd.props) {
            double x = 0;
            if (k == "to") {
                const auto a = aspect_by_key(v);
                if (!a || *a == Neutral) r.diags.push_back({nd.line, "неизвестный аспект «" + v + "» (fire, earth, air, water)"});
            } else if (!parse_number(v, x)) {
                r.diags.push_back({nd.line, "параметр " + k + " руны «" + nd.name + "» — не число: «" + v + "»"});
            } else if ((k == "use" || k == "share") && (x < 0 || x > 1)) {
                r.diags.push_back({nd.line, "параметр " + k + " руны «" + nd.name + "» должен быть от 0 до 1"});
            } else if (x < 0) {
                r.diags.push_back({nd.line, "параметр " + k + " руны «" + nd.name + "» отрицательный"});
            }
        }
        if (nd.cls == "Convert" && !nd.props.count("to")) r.diags.push_back({nd.line, "у Convert нужен параметр to=аспект"});
    }
    if (!r.ok()) return r;

    // Разрыв дуги: поток входит в руну, но у руны нет питающего потока; источник не принимает поток.
    bool any_source = false, any_effect = false;
    for (int i = 0; i < n; ++i) {
        const Node& nd = c.nodes[i];
        if (nd.info->domain != Domain::Flow) continue;
        if (nd.cls == "Condenser") {
            any_source = true;
            if (fin[i]) r.diags.push_back({nd.line, "в Condenser «" + nd.name + "» ничего не втекает: он сам источник"});
        } else if (!fin[i]) {
            r.diags.push_back({nd.line, "разрыв дуги: в руну «" + nd.name + "» не втекает поток (нет пути от источника)"});
        }
        if (nd.cls == "Matter" || nd.cls == "Heat" || nd.cls == "Impulse" || nd.cls == "Shape") any_effect = true;
    }
    if (!any_source) r.diags.push_back({0, "в схеме нет источника (Condenser): нечем питать эффект"});
    if (!any_effect) r.diags.push_back({0, "в схеме нет эффектора (Matter, Heat, Impulse, Shape): эффекту некуда выйти"});

    // Петель нет: топологический порядок по потоку и сигналу вместе.
    std::vector<int> indeg(n, 0);
    std::vector<std::vector<int>> adj(n);
    for (auto [a, bb] : c.flow) { adj[a].push_back(bb); ++indeg[bb]; }
    for (auto [a, bb] : c.sig) { adj[a].push_back(bb); ++indeg[bb]; }
    std::vector<int> order, q;
    for (int i = 0; i < n; ++i) if (!indeg[i]) q.push_back(i);
    while (!q.empty()) {
        const int v = q.back(); q.pop_back();
        order.push_back(v);
        for (int w : adj[v]) if (--indeg[w] == 0) q.push_back(w);
    }
    if (static_cast<int>(order.size()) != n) {
        std::string who;
        for (int i = 0; i < n; ++i) if (indeg[i] > 0) { who = c.nodes[i].name; break; }
        r.diags.push_back({0, "в схеме петля (поток или сигнал возвращается к «" + who + "»): петель в круге нет"});
    }
    std::sort(r.diags.begin(), r.diags.end(), [](const Diag& a, const Diag& b2) { return a.line < b2.line; });
    return r;
}

// ------------------------------------------------------------------------------------------------ симуляция

struct Environment {
    Mix mix{};
    double relax = 0.08, saturation = 30;

    Mix draw(double want) {
        const double have = total(mix);
        if (have <= 0 || want <= 0) return {};
        const double take = std::min(want, have);
        Mix out = mix * (take / have);
        for (int i = 0; i < AspectCount; ++i) mix[i] -= out[i];
        return out;
    }
    void deposit(const Mix& m) {
        mix += m;
    }
    /// Окраска выцветает в нейтральную.
    void relax_step() {
        for (int a = Fire; a < AspectCount; ++a) {
            const double d = mix[a] * relax;
            mix[a] -= d;
            mix[Neutral] += d;
        }
    }
};

struct Effects {
    double matter = 0, heat = 0, impulse = 0, shape = 0;
};

enum class Outcome { Done, Exploded, Fizzled };

struct CastResult {
    Outcome outcome = Outcome::Done;
    std::string why;
    int ticks = 0;
    double ambient_drawn = 0;      ///< мана среды, ушедшая в поток
    double personal_spent = 0;     ///< личная мана (сигнал, затворы, «батарейка»)
    double battery = 0;            ///< часть траты личной маны, не заменённая средой
    Effects fx;
    double effect_total = 0;
    Mix residue_before{}, residue_after{};
    std::vector<std::string> events;
};

double effect_of(const Effects& e) { return e.matter + e.heat + e.impulse + e.shape; }

class Caster {
public:
    explicit Caster(const Circuit& c, Environment& env, bool trace) : c_(c), env_(env), trace_(trace) {
        const int n = static_cast<int>(c.nodes.size());
        out_.assign(n, Mix{});
        store_.assign(n, Mix{});
        sigv_.assign(n, 0);
        hist_.assign(n, {});
        count_.assign(n, 0);
        prev_.assign(n, 0);
        outdeg_.assign(n, 0);
        for (auto [a, b] : c.flow) ++outdeg_[a];
        in_flow_.assign(n, {});
        in_sig_.assign(n, {});
        for (auto [a, b] : c.flow) in_flow_[b].push_back(a);
        for (auto [a, b] : c.sig) in_sig_[b].push_back(a);
        // Порядок: топологический (уже проверен компиляцией).
        std::vector<int> indeg(n, 0);
        std::vector<std::vector<int>> adj(n);
        for (auto [a, b] : c.flow) { adj[a].push_back(b); ++indeg[b]; }
        for (auto [a, b] : c.sig) { adj[a].push_back(b); ++indeg[b]; }
        std::deque<int> q;
        for (int i = 0; i < n; ++i) if (!indeg[i]) q.push_back(i);
        while (!q.empty()) {
            const int v = q.front(); q.pop_front();
            order_.push_back(v);
            for (int w : adj[v]) if (--indeg[w] == 0) q.push_back(w);
        }
    }

    CastResult run() {
        CastResult r;
        r.residue_before = env_.mix;
        const double gain = c_.skill * c_.tools * c_.purity;
        double personal_left = c_.personal;
        for (int t = 0; t < c_.header.time; ++t) {
            r.ticks = t + 1;
            double tick_personal = 0;
            Mix tick_waste{};
            // Сигналы.
            for (int v : order_) {
                const Node& nd = c_.nodes[v];
                if (nd.info->domain != Domain::Signal) continue;
                int val = 0;
                const auto& ins = in_sig_[v];
                auto all = [&] { for (int s : ins) if (!sigv_[s]) return 0; return 1; };
                auto any = [&] { for (int s : ins) if (sigv_[s]) return 1; return 0; };
                if (nd.cls == "Touch") val = t >= static_cast<int>(c_.touch_at);
                else if (nd.cls == "Target") val = c_.target_seen;
                else if (nd.cls == "Timer") val = t >= static_cast<int>(nd.num("after", 0));
                else if (nd.cls == "Distance") val = c_.distance < nd.num("lt", 0);
                else if (nd.cls == "And") val = ins.empty() ? 0 : all();
                else if (nd.cls == "Or") val = any();
                else if (nd.cls == "Not") val = !any();
                else if (nd.cls == "Delay") {
                    const int d = static_cast<int>(nd.num("ticks", 1));
                    hist_[v].push_back(any());
                    val = static_cast<int>(hist_[v].size()) > d ? hist_[v][hist_[v].size() - 1 - d] : 0;
                } else if (nd.cls == "Counter") {
                    const int in = any();
                    if (in && !prev_[v]) ++count_[v];
                    prev_[v] = in;
                    val = count_[v] >= static_cast<int>(nd.num("count", 1));
                }
                sigv_[v] = val;
                tick_personal += 0.05;   // сигнал стоит чуть-чуть личной маны
            }
            // Поток.
            for (int v : order_) {
                const Node& nd = c_.nodes[v];
                if (nd.info->domain != Domain::Flow) continue;
                Mix in{};
                for (int p : in_flow_[v]) in += out_[p] * (1.0 / std::max(1, outdeg_[p]));
                bool open = true;
                for (int s : in_sig_[v]) open = open && sigv_[s];
                Mix out{};
                const double limit = nd.num("limit", default_limit);
                const double in_total = total(in);

                if (nd.cls == "Condenser") {
                    if (open) {
                        const double want = nd.num("power", 10);
                        if (want > limit) {
                            out_[v] = Mix{};
                            return explode(r, nd, "конденсатор «" + nd.name + "» тянет " + fmt(want) + " > предела " + fmt(limit), want - limit, t);
                        }
                        out = env_.draw(want);
                        const double got = total(out);
                        r.ambient_drawn += got;
                        tick_personal += got / gain;
                        if (got < want) {                 // среды не хватило: «от батарейки» — личная мана 1:1
                            out[Neutral] += want - got;
                            tick_personal += want - got;
                            r.battery += want - got;
                        }
                    }
                } else if (nd.cls == "Amplifier") {
                    out = in;
                    if (open) {
                        const double ratio = nd.num("ratio", 2);
                        const Mix extra = env_.draw(in_total * (ratio - 1));
                        out += extra;
                        r.ambient_drawn += total(extra);
                        tick_personal += total(extra) / (gain * std::max(1.0, ratio));
                    } else tick_waste += in;
                } else if (nd.cls == "Capacitor") {
                    store_[v] += in;
                    const double cap = nd.num("cap", 40), rate = nd.num("rate", 10);
                    if (open) {
                        const double have = total(store_[v]);
                        if (have > cap) return explode(r, nd, "накопитель «" + nd.name + "» переполнен: " + fmt(have) + " > " + fmt(cap), have - cap, t);
                        const double give = std::min(have, rate);
                        if (have > 0) { out = store_[v] * (give / have); store_[v] += out * -1.0; }
                    }
                } else if (nd.cls == "Convert") {
                    if (!open) { tick_waste += in; }
                    else {
                        const Aspect to = static_cast<Aspect>(static_cast<int>(nd.num("to", Fire)));
                        const double share = nd.num("share", 1);
                        for (int a = 0; a < AspectCount; ++a) {
                            const double moved = in[a] * share;
                            const double loss = moved * conversion_loss(static_cast<Aspect>(a), to);
                            out[a] += in[a] - moved;
                            out[to] += moved - loss;
                            Mix back{};
                            back[to] = loss;      // потери возвращаются в среду окрашенными
                            tick_waste += back;
                        }
                    }
                } else if (nd.cls == "Matter" || nd.cls == "Heat" || nd.cls == "Impulse" || nd.cls == "Shape") {
                    if (!open) { tick_waste += in; }
                    else {
                        const double use = nd.num("use", 1);
                        const Mix taken = in * use;
                        out = in - taken;
                        double eff = 0;
                        const double tt = total(taken);
                        if (nd.cls == "Matter") eff = taken[Earth] + 0.25 * (tt - taken[Earth]);
                        else if (nd.cls == "Heat") eff = taken[Fire] + 0.25 * (tt - taken[Fire]);
                        else eff = tt;
                        (nd.cls == "Matter" ? r.fx.matter : nd.cls == "Heat" ? r.fx.heat : nd.cls == "Impulse" ? r.fx.impulse : r.fx.shape) += eff;
                        // Непрофильная мана, не ставшая эффектом, возвращается в среду в своих аспектах.
                        if (tt > 0) tick_waste += taken * ((tt - eff) / tt);
                    }
                } else {   // In, Out, Pass
                    out = in;
                }
                if (total(out) > limit) return explode(r, nd, "в руну «" + nd.name + "» течёт " + fmt(total(out)) + " > предела " + fmt(limit), total(out) - limit, t);
                out_[v] = out;
                if (outdeg_[v] == 0 && nd.info->domain == Domain::Flow && !is_effector(nd)) tick_waste += out;
                if (outdeg_[v] == 0 && is_effector(nd)) tick_waste += out;
            }
            env_.deposit(tick_waste);
            r.personal_spent += tick_personal;
            personal_left -= tick_personal;
            if (trace_) {
                std::printf("    такт %2d: личная %.2f, поток:", t, tick_personal);
                for (int v : order_)
                    if (c_.nodes[v].info->domain == Domain::Flow) std::printf(" %s=%.1f", c_.nodes[v].name.c_str(), total(out_[v]));
                std::printf("\n");
            }
            if (personal_left < 0) {
                r.outcome = Outcome::Fizzled;
                r.why = "личная мана кончилась на такте " + std::to_string(t);
                break;
            }
        }
        for (const Mix& s : store_) env_.deposit(s);   // что осталось в накопителях, уходит в среду
        finish(r);
        return r;
    }

private:
    static bool is_effector(const Node& n) { return n.cls == "Matter" || n.cls == "Heat" || n.cls == "Impulse" || n.cls == "Shape"; }
    static std::string fmt(double v) { char b[32]; std::snprintf(b, sizeof b, "%.1f", v); return b; }

    CastResult explode(CastResult& r, const Node& nd, const std::string& why, double excess, int t) {
        r.outcome = Outcome::Exploded;
        r.why = why + " (такт " + std::to_string(t) + ")";
        r.events.push_back("ВЗРЫВ: схема сорвалась, " + fmt(excess * 3) + " ед. энергии вырвалось в кольцо");
        // Всё, что текло и лежало в накопителях, — обратно в среду.
        for (const Mix& m : out_) env_.deposit(m);
        for (const Mix& m : store_) env_.deposit(m);
        (void)nd;
        finish(r);
        return r;
    }

    void finish(CastResult& r) {
        r.effect_total = effect_of(r.fx);
        r.residue_after = env_.mix;
        for (int a = Fire; a < AspectCount; ++a)
            if (env_.mix[a] > env_.saturation) {
                static constexpr const char* what[AspectCount] = {"", "огонь поджигает: трава вокруг начинает тлеть сама",
                                                                  "земля кристаллизуется", "воздух закручивается ветром",
                                                                  "вода конденсируется"};
                r.events.push_back(std::string("насыщение (") + aspect_names[a] + " " + fmt(env_.mix[a]) + "): " + what[a]);
            }
    }

    const Circuit& c_;
    Environment& env_;
    bool trace_;
    std::vector<int> order_, outdeg_, count_, prev_, sigv_;
    std::vector<Mix> out_, store_;
    std::vector<std::deque<int>> hist_;
    std::vector<std::vector<int>> in_flow_, in_sig_;
};

// ------------------------------------------------------------------------------------------------ вывод и запуск

void print_mix(const char* label, const Mix& m) {
    std::printf("    %s:", label);
    for (int a = 0; a < AspectCount; ++a)
        if (m[a] > 0.05) std::printf(" %s %.1f;", aspect_names[a], m[a]);
    std::printf(" всего %.1f\n", total(m));
}

const char* outcome_name(Outcome o) {
    switch (o) {
        case Outcome::Done: return "готово";
        case Outcome::Exploded: return "ВЗРЫВ";
        case Outcome::Fizzled: return "срыв";
    }
    return "?";
}

void print_cast(int n, const CastResult& r) {
    std::printf("  каст %d — %s", n, outcome_name(r.outcome));
    if (!r.why.empty()) std::printf(": %s", r.why.c_str());
    std::printf("\n");
    std::printf("    тактов %d; мана среды в потоке %.1f; личная мана %.2f", r.ticks, r.ambient_drawn, r.personal_spent);
    if (r.personal_spent > 0) std::printf("; усиление %.1f×", r.ambient_drawn / r.personal_spent);
    std::printf("\n");
    std::printf("    эффект: материя %.1f, нагрев %.1f, импульс %.1f, форма %.1f (всего %.1f", r.fx.matter, r.fx.heat, r.fx.impulse, r.fx.shape, r.effect_total);
    if (r.ambient_drawn + r.battery > 0) std::printf(", %.0f%% потока", 100 * r.effect_total / (r.ambient_drawn + r.battery));
    std::printf(")\n");
    print_mix("среда до  ", r.residue_before);
    print_mix("среда после", r.residue_after);
    for (const std::string& e : r.events) std::printf("    * %s\n", e.c_str());
}

void print_diags(const std::string& file, const std::vector<Diag>& d) {
    for (const Diag& x : d) {
        if (x.line) std::printf("  %s:%d: %s\n", file.c_str(), x.line, x.text.c_str());
        else std::printf("  %s: %s\n", file.c_str(), x.text.c_str());
    }
}

std::optional<std::string> read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return std::nullopt;
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

struct Options {
    std::vector<std::string> files;
    int casts = 2, gap = 0;
    bool trace = false, check = false;
};

/// Исполнить файлы в одном общем мире. Возвращает число файлов с ошибкой разбора.
int run_files(const Options& o, std::vector<CastResult>* log = nullptr) {
    Environment env;
    bool env_ready = false;
    int bad = 0;
    for (const std::string& f : o.files) {
        const auto text = read_file(f);
        if (!text) { std::printf("%s: файл не открывается\n", f.c_str()); ++bad; continue; }
        const std::string base = std::filesystem::path(f).filename().string();
        std::printf("== %s\n", base.c_str());
        Compiled c = compile(*text);
        if (!c.ok()) {
            std::printf("  ошибки схемы (%zu):\n", c.diags.size());
            print_diags(base, c.diags);
            ++bad;
            continue;
        }
        const Circuit& ci = c.circuit;
        if (!env_ready || ci.reset) {   // мир берётся из первой схемы и дальше общий (остаток прошлого заклинания остаётся); reset=1 — начать заново
            env.mix = ci.world;
            env.relax = ci.relax;
            env.saturation = ci.saturation;
            env_ready = true;
        }
        std::printf("  заклинание «%s»: %zu рун, кольцо %.0f м, %d тактов; усиление навыка %.2f×\n", ci.name.c_str(), ci.nodes.size(), ci.header.ring,
                    ci.header.time, ci.skill * ci.tools * ci.purity);
        for (int k = 1; k <= o.casts; ++k) {
            if (k > 1) for (int g = 0; g < o.gap; ++g) env.relax_step();
            Caster cast(ci, env, o.trace);
            const CastResult r = cast.run();
            print_cast(k, r);
            if (log) log->push_back(r);
        }
    }
    return bad;
}

// ------------------------------------------------------------------------------------------------ самопроверка

int g_failed = 0;
void expect(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "НЕТ", what);
    if (!ok) ++g_failed;
}

std::string dir() { return RUNE_CIRCUIT_DIR; }

bool has_diag(const Compiled& c, const char* sub) {
    for (const Diag& d : c.diags)
        if (d.text.find(sub) != std::string::npos) return true;
    return false;
}

int self_check() {
    std::printf("RuneCircuit --check\n");
    auto load = [&](const char* name) { return read_file(dir() + "/" + name).value_or(""); };

    // 1. Все примеры, кроме заведомо сломанного, собираются.
    for (const char* f : {"meteor", "fireball", "iceball", "nested", "logic", "overload", "battery"}) {
        const Compiled c = compile(load((std::string(f) + ".circle").c_str()));
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s.circle собирается", f);
        expect(c.ok(), buf);
    }

    // 2. Сломанная схема: диагнозы по строкам, а не падение.
    {
        const Compiled c = compile(load("broken.circle"));
        expect(!c.ok(), "broken.circle отвергается");
        expect(has_diag(c, "неизвестный аспект"), "  — несколько диагнозов сразу");
        expect(compile("spell A\nrune x = Wibble\n").diags.size() == 1, "  — неизвестный класс");
        const Compiled c2 = compile("spell A time=4\nrune c = Condenser\nrune f = Convert to=lava\nrune i = Impulse\nflow c -> f -> i\n");
        expect(has_diag(c2, "неизвестный аспект"), "  — неизвестный аспект");
        const Compiled c3 = compile("spell A time=4\nrune c = Condenser\nrune i = Impulse\nrune j = Impulse\nflow c -> i\n");
        expect(has_diag(c3, "разрыв дуги"), "  — разрыв дуги: руна без питающего потока");
        const Compiled c4 = compile("spell A time=4\nrune c = Condenser\nrune f = Convert fire\nrune i = Impulse\nflow c -> f -> i\nflow i -> f\n");
        expect(has_diag(c4, "петля"), "  — петля");
        const Compiled c5 = compile("spell A time=4\nrune s = Touch\nrune c = Condenser\nrune i = Impulse\nflow c -> i\nflow s -> i\n");
        expect(has_diag(c5, "работает с сигналом"), "  — сигнал по каналу потока");
        const Compiled c6 = compile("spell A time=4\nrune c = Condenser\nrune f = Convert fire\nrune i = Impulse\nflow c -> f -> i\nsignal c ~> i\n");
        expect(has_diag(c6, "руна потока"), "  — поток по каналу сигнала");
        const Compiled c7 = compile("spell A time=4\nrune c = Condenser\nrune i = Impulse\nflow c -> i\ncall x = Nope\n");
        expect(has_diag(c7, "нет круга"), "  — вызов неизвестного круга");
    }

    // 3. Вложенный круг раскрывается в плоскую схему.
    {
        const Compiled c = compile(load("nested.circle"));
        bool inner = false;
        for (const Node& n : c.circuit.nodes) inner = inner || n.name == "b.h";
        expect(c.ok() && inner, "вложенный круг: руны вызова получают имена «b.h»");
        std::vector<CastResult> log;
        Environment env; env.mix = c.circuit.world;
        Caster k(c.circuit, env, false);
        const CastResult r = k.run();
        expect(r.outcome == Outcome::Done && r.fx.heat > 0, "вложенный круг даёт нагрев");
    }

    // 4. Метеорит: питание, затвор, усиление.
    const Compiled meteor = compile(load("meteor.circle"));
    CastResult m1, m2;
    {
        Environment env; env.mix = meteor.circuit.world; env.relax = meteor.circuit.relax; env.saturation = meteor.circuit.saturation;
        Caster a(meteor.circuit, env, false);
        m1 = a.run();
        Caster b(meteor.circuit, env, false);
        m2 = b.run();
    }
    expect(m1.outcome == Outcome::Done, "метеорит выполняется");
    expect(m1.fx.matter > 0 && m1.fx.heat > 0 && m1.fx.impulse > 0, "метеорит: камень, нагрев и импульс");
    expect(m1.ambient_drawn / m1.personal_spent > 5, "усиление: мана среды >> личная (транзистор)");
    expect(m1.personal_spent < 0.2 * m1.ambient_drawn, "личная мана — малая доля потока");
    expect(m2.residue_before[Earth] > 1 && m2.residue_before[Fire] > 1, "остаток окрашен в землю и огонь");
    expect(m2.effect_total > m1.effect_total, "второй метеорит здесь дешевле: тот же расход даёт больше эффекта");

    // 5. Затвор: без сигнала поток не идёт.
    {
        Circuit c = meteor.circuit;
        c.touch_at = 1000;   // сенсор не сработает
        Environment env; env.mix = c.world;
        Caster k(c, env, false);
        const CastResult r = k.run();
        expect(r.ambient_drawn == 0 && r.effect_total == 0, "сигнал не пришёл — поток закрыт, эффекта нет");
    }

    // 6. Аспекты: огонь после земли дёшево, воды после огня дорого.
    {
        const Compiled fb = compile(load("fireball.circle")), ib = compile(load("iceball.circle"));
        auto run_ice = [&](bool after_fire) {
            Environment env; env.mix = ib.circuit.world; env.relax = 0.08; env.saturation = 1e9;
            if (after_fire) { Caster k(fb.circuit, env, false); k.run(); }
            Caster k(ib.circuit, env, false);
            return k.run();
        };
        const CastResult clean = run_ice(false), dirty = run_ice(true);
        expect(clean.effect_total > 0, "ледяной шар в чистой среде работает");
        expect(dirty.effect_total < clean.effect_total, "остаток огня мешает ледяному заклинанию: порядок кастов — тактика");
        Environment env; env.mix = fb.circuit.world; env.relax = 0.08; env.saturation = 1e9;
        Caster k1(fb.circuit, env, false); const CastResult f1 = k1.run();
        Caster k2(fb.circuit, env, false); const CastResult f2 = k2.run();
        expect(f2.effect_total > f1.effect_total, "огонь после огня дешевле, чем в чистой среде");
    }

    // 7. Релаксация и насыщение.
    {
        Environment env; env.mix = {};
        env.mix[Neutral] = 100; env.mix[Fire] = 50; env.relax = 0.2; env.saturation = 30;
        const double before = env.mix[Fire];
        for (int i = 0; i < 5; ++i) env.relax_step();
        expect(env.mix[Fire] < before * 0.4 && std::fabs(total(env.mix) - 150) < 1e-9, "окраска выцветает в нейтральную, мана не теряется");
        const Compiled fb = compile(load("fireball.circle"));
        Circuit c = fb.circuit;
        c.world[Fire] = 40;   // пересыщенная среда
        Environment e2; e2.mix = c.world; e2.relax = 0.0; e2.saturation = 30;
        Caster k(c, e2, false);
        const CastResult r = k.run();
        bool ignite = false;
        for (const std::string& e : r.events) ignite = ignite || e.find("тлеть") != std::string::npos;
        expect(ignite, "избыток огня поджигает траву: заклинание оставляет живой след");
    }

    // 8. Перегрузка, «батарейка», логика.
    {
        const Compiled c = compile(load("overload.circle"));
        Environment env; env.mix = c.circuit.world;
        Caster k(c.circuit, env, false);
        const CastResult r = k.run();
        expect(r.outcome == Outcome::Exploded, "перегрузка: срыв и взрыв");
        const Compiled b = compile(load("battery.circle"));
        Environment e2; e2.mix = b.circuit.world;
        Caster kb(b.circuit, e2, false);
        const CastResult rb = kb.run();
        expect(rb.battery > 0 && rb.ambient_drawn < rb.battery, "без маны среды — «от батарейки»");
        expect(rb.personal_spent / (rb.ambient_drawn + rb.battery) > 3 * m1.personal_spent / m1.ambient_drawn, "«от батарейки» дороже, чем от среды");
        const Compiled l = compile(load("logic.circle"));
        Environment e3; e3.mix = l.circuit.world;
        Caster kl(l.circuit, e3, false);
        const CastResult rl = kl.run();
        expect(rl.outcome == Outcome::Done && rl.ambient_drawn > 0 && rl.ambient_drawn < 14 * 10, "логика И: заклинание срабатывает не с первого такта");
        Circuit far = l.circuit;
        far.distance = 50;
        Environment e4; e4.mix = far.world;
        Caster kf(far, e4, false);
        expect(kf.run().ambient_drawn == 0, "логика И: цель далеко — затвор закрыт");
    }

    // 9. Детерминизм.
    {
        auto digest = [&] {
            Environment env; env.mix = meteor.circuit.world;
            Caster a(meteor.circuit, env, false), b(meteor.circuit, env, false);
            const CastResult x = a.run(), y = b.run();
            return std::make_tuple(x.ambient_drawn, x.personal_spent, y.effect_total, env.mix);
        };
        expect(digest() == digest(), "детерминизм: два прогона совпадают");
    }

    std::printf(g_failed == 0 ? "\nOK\n" : "\nОШИБКА: %d проверок не прошли\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](int& v) { if (i + 1 < argc) v = std::atoi(argv[++i]); };
        if (a == "--check") o.check = true;
        else if (a == "--trace") o.trace = true;
        else if (a == "--casts") next(o.casts);
        else if (a == "--gap") next(o.gap);
        else if (a == "-h" || a == "--help") {
            std::printf("RuneCircuit [файл.circle ...] [--casts N] [--gap T] [--trace] | --check\n");
            return 0;
        } else o.files.push_back(a);
    }
    if (o.check) return self_check();
    const bool demo = o.files.empty();
    if (demo) {
        for (const char* f : {"meteor", "fireball", "iceball", "nested", "logic", "overload", "battery", "broken"})
            o.files.push_back(dir() + "/" + f + ".circle");
    }
    return run_files(o) == 0 || demo ? 0 : 1;
}
