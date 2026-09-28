/**
 * @example 04_runtime_schema.cpp
 * Работа с событиями без C++-типов — так их будут видеть скрипты заклинаний,
 * моды и отладочный инспектор. Событие описывается схемой (аналог VAO),
 * отправляется байтами и читается по именам полей.
 */

#include <EventSystem/EventSystem.hpp>

#include <cstdint>
#include <cstring>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace es = EventSystem;

namespace {

/// Значение поля как строка — так инспектор или отладчик скриптов показывает событие.
std::string format_field(const es::FieldDesc& field, const std::byte* data) {
    auto read = [data]<typename T>(T value) {
        std::memcpy(&value, data, sizeof(T));
        return value;
    };
    switch (field.kind) {
        case es::FieldKind::UInt32: return std::to_string(read(std::uint32_t{}));
        case es::FieldKind::Int32: return std::to_string(read(std::int32_t{}));
        case es::FieldKind::Float32: return std::to_string(read(float{}));
        case es::FieldKind::Bool: return read(bool{}) ? "true" : "false";
        default: return std::format("<{} bytes>", field.size);
    }
}

/// Печатает все события канала, зная только его схему.
void inspect(const es::IChannel& channel) {
    const es::EventSchema& schema = channel.schema();
    const es::EventBuffer& events = channel.readable();
    std::println("[{}] layout={} events={}", schema.name, es::to_string(schema.layout), events.size());
    for (std::size_t i = 0; i < events.size(); ++i) {
        std::string line;
        for (std::size_t f = 0; f < schema.fields.size(); ++f) {
            line += std::format(" {}:{}={}", schema.fields[f].name, es::to_string(schema.fields[f].kind),
                                format_field(schema.fields[f], events.field_data(i, f)));
        }
        std::println("  #{}{}", i, line);
    }
}

/// Мини-«построитель события» для скрипта: пишет значения по именам полей.
class ScriptEvent {
public:
    explicit ScriptEvent(const es::EventSchema& schema) : m_schema(schema), m_bytes(schema.size) {}

    template<typename T>
    ScriptEvent& set(std::string_view name, T value) {
        const es::FieldDesc* field = m_schema.find_field(name);
        if (field == nullptr || field->size != sizeof(T)) {
            throw es::EventSystemError(std::format("script: bad field '{}'", name));
        }
        std::memcpy(m_bytes.data() + field->offset, &value, sizeof(T));
        return *this;
    }

    [[nodiscard]] const std::byte* data() const noexcept { return m_bytes.data(); }

private:
    const es::EventSchema& m_schema;
    std::vector<std::byte> m_bytes;
};

} // namespace

int main() {
    // Схема, пришедшая из данных мода: «fireball» с тремя полями.
    const es::EventSchema fireball{
        .name = "mod.fireball_exploded",
        .id = es::make_event_id("mod.fireball_exploded"),
        .size = 12,
        .alignment = 4,
        .layout = es::Layout::SoA,
        .fields = {
            es::FieldDesc{.name = "caster", .kind = es::FieldKind::UInt32, .offset = 0, .size = 4, .alignment = 4},
            es::FieldDesc{.name = "radius", .kind = es::FieldKind::Float32, .offset = 4, .size = 4, .alignment = 4},
            es::FieldDesc{.name = "power", .kind = es::FieldKind::Int32, .offset = 8, .size = 4, .alignment = 4},
        },
    };

    es::EventBus bus;
    es::IChannel& channel = bus.register_schema(fireball);
    bus.declare_module("ModScripts").produces(fireball.id);

    // Скрипт отправляет события, зная только имена полей.
    ScriptEvent event(channel.schema());
    channel.emit_raw(event.set("caster", std::uint32_t{7}).set("radius", 3.5f).set("power", 120).data());
    channel.emit_raw(event.set("caster", std::uint32_t{8}).set("radius", 1.0f).set("power", 15).data());
    bus.advance_tick();

    // Инспектор обходит все каналы шины — и C++-события, и события модов.
    for (std::size_t i = 0; i < bus.channel_count(); ++i) {
        inspect(bus.channel_at(i));
    }

    // Неверная схема отклоняется при регистрации с понятной причиной.
    es::EventSchema broken = fireball;
    broken.name = "mod.broken";
    broken.id = es::make_event_id(broken.name);
    broken.size = 16; // 4 байта не описаны полями
    try {
        bus.register_schema(broken);
    } catch (const es::EventSystemError& error) {
        std::println("rejected: {}", error.what());
    }
}
