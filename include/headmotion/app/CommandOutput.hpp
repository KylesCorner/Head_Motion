#pragma once

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <memory>
#include <fstream>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace headmotion::app {

/*
 * A single structured command event.
 *
 * The CLI serializes these as one JSON object per line (JSONL).  The GUI can
 * later consume the same CommandEvent objects directly without parsing stdout.
 */
struct CommandEvent {
    using Value = std::variant<
        std::string,
        std::int64_t,
        std::uint64_t,
        double,
        bool
    >;

    struct Field {
        std::string key;
        Value value;

        Field(std::string key, std::string value)
            : key(std::move(key)),
              value(std::move(value)) {
        }

        Field(std::string key, const char* value)
            : key(std::move(key)),
              value(
                  std::string(
                      value != nullptr
                          ? value
                          : ""
                  )
              ) {
        }

        Field(std::string key, std::int64_t value)
            : key(std::move(key)),
              value(value) {
        }

        Field(std::string key, std::uint64_t value)
            : key(std::move(key)),
              value(value) {
        }

        Field(std::string key, std::uint32_t value)
            : key(std::move(key)),
              value(
                  static_cast<std::uint64_t>(
                      value
                  )
              ) {
        }

        Field(std::string key, std::uint16_t value)
            : key(std::move(key)),
              value(
                  static_cast<std::uint64_t>(
                      value
                  )
              ) {
        }

        Field(std::string key, std::uint8_t value)
            : key(std::move(key)),
              value(
                  static_cast<std::uint64_t>(
                      value
                  )
              ) {
        }

        Field(std::string key, int value)
            : key(std::move(key)),
              value(
                  static_cast<std::int64_t>(
                      value
                  )
              ) {
        }

        Field(std::string key, double value)
            : key(std::move(key)),
              value(value) {
        }

        Field(std::string key, float value)
            : key(std::move(key)),
              value(
                  static_cast<double>(
                      value
                  )
              ) {
        }

        Field(std::string key, bool value)
            : key(std::move(key)),
              value(value) {
        }
    };

    std::string type;
    std::string command;
    std::vector<Field> fields;
};


/*
 * CommandOutput
 * -------------
 *
 * Per-operation output object.
 *
 * IMPORTANT FOR PARALLELISM:
 * This class never replaces std::cout/std::cerr stream buffers and never uses
 * process-global output redirection.
 *
 * Each command instance owns or receives its own CommandOutput.  Multiple
 * device operations can therefore emit events concurrently without one
 * operation hijacking another operation's output.
 */
class CommandOutput {
public:
    using Field = CommandEvent::Field;
    using Sink =
        std::function<void(
            const CommandEvent&
        )>;

    /*
     * Default command-line behavior: write JSONL to stdout.
     */
    explicit CommandOutput(
        std::string command
    )
        : CommandOutput(
              std::move(command),
              jsonStdoutSink()
          ) {
    }

    /*
     * GUI/test behavior: inject a per-operation event sink.
     */
    CommandOutput(
        std::string command,
        Sink sink
    )
        : command_(std::move(command)),
          sink_(std::move(sink)) {
    }

    const std::string& command() const noexcept {
        return command_;
    }

    void event(
        const std::string& event_type,
        std::initializer_list<Field> fields = {}
    ) {
        CommandEvent event;
        event.type = event_type;
        event.command = command_;
        event.fields.assign(
            fields.begin(),
            fields.end()
        );

        emit(event);
    }

    void started(
        std::initializer_list<Field> fields = {}
    ) {
        event(
            "started",
            fields
        );
    }

    void status(
        const std::string& stage
    ) {
        event(
            "status",
            {
                {"stage", stage}
            }
        );
    }

    void progress(
        std::uint32_t entries_left,
        std::uint32_t total_entries
    ) {
        const std::uint64_t entries_done =
            total_entries >= entries_left
                ? static_cast<std::uint64_t>(
                      total_entries -
                      entries_left
                  )
                : 0;

        const double percent =
            total_entries == 0
                ? 0.0
                : 100.0 *
                      static_cast<double>(
                          entries_done
                      ) /
                      static_cast<double>(
                          total_entries
                      );

        event(
            "progress",
            {
                {
                    "entries_done",
                    entries_done
                },
                {
                    "entries_left",
                    static_cast<std::uint64_t>(
                        entries_left
                    )
                },
                {
                    "entries_total",
                    static_cast<std::uint64_t>(
                        total_entries
                    )
                },
                {
                    "percent",
                    percent
                }
            }
        );
    }

    void outputFile(
        const std::string& type,
        const std::string& path
    ) {
        event(
            "output",
            {
                {"type", type},
                {"path", path}
            }
        );
    }

    void warning(
        const std::string& code,
        const std::string& message
    ) {
        event(
            "warning",
            {
                {"code", code},
                {"message", message}
            }
        );
    }

    void error(
        const std::string& code,
        const std::string& message
    ) {
        event(
            "error",
            {
                {"code", code},
                {"message", message}
            }
        );
    }

    void completed(
        bool success,
        int exit_code
    ) {
        event(
            "completed",
            {
                {"success", success},
                {"exit_code", exit_code}
            }
        );
    }

    /*
     * Useful for tests, subprocess adapters, and the CLI.
     */
    static std::string toJsonLine(
        const CommandEvent& event
    ) {
        std::ostringstream json;

        json
            << "{"
            << "\"event\":"
            << quote(event.type)
            << ",\"command\":"
            << quote(event.command);

        for (const auto& field : event.fields) {
            json
                << ","
                << quote(field.key)
                << ":";

            std::visit(
                [&json](const auto& value) {
                    using T =
                        std::decay_t<
                            decltype(value)
                        >;

                    if constexpr (
                        std::is_same_v<
                            T,
                            std::string
                        >
                    ) {
                        json
                            << quote(value);
                    }
                    else if constexpr (
                        std::is_same_v<
                            T,
                            bool
                        >
                    ) {
                        json
                            << (
                                value
                                    ? "true"
                                    : "false"
                            );
                    }
                    else {
                        json << value;
                    }
                },
                field.value
            );
        }

        json << "}";
        return json.str();
    }

    static Sink jsonStdoutSink() {
        return [](
            const CommandEvent& event
        ) {
            const std::string line =
                toJsonLine(event);

            /*
             * std::cout itself is process-global, but we only use it as the
             * final CLI sink.  We never replace its rdbuf.  This mutex simply
             * guarantees one complete JSON object per line if commands happen
             * to emit concurrently.
             */
            std::lock_guard<std::mutex> lock(
                stdoutMutex()
            );

            std::cout
                << line
                << '\n'
                << std::flush;
        };
    }

    /*
     * Create a JSONL sink that owns its file for the lifetime of the sink.
     *
     * The file is opened with std::ios::trunc, so starting a new run with the
     * same path automatically replaces the previous run's log.
     */
    static Sink jsonFileSink(
        const std::filesystem::path& path
    ) {
        if (path.has_parent_path()) {
            std::filesystem::create_directories(
                path.parent_path()
            );
        }

        auto file =
            std::make_shared<std::ofstream>(
                path,
                std::ios::out |
                std::ios::trunc
            );

        if (!*file) {
            throw std::runtime_error(
                "Failed to open JSONL log file: " +
                path.string()
            );
        }

        auto mutex =
            std::make_shared<std::mutex>();

        return [
            file = std::move(file),
            mutex = std::move(mutex)
        ](
            const CommandEvent& event
        ) {
            const std::string line =
                toJsonLine(event);

            std::lock_guard<std::mutex> lock(
                *mutex
            );

            *file
                << line
                << '\n';

            file->flush();

            if (!*file) {
                throw std::runtime_error(
                    "Failed while writing JSONL command log"
                );
            }
        };
    }

    /*
     * Fan one CommandEvent out to multiple destinations.
     *
     * The GUI uses this to send each event both to its in-memory device state
     * and to a JSONL file without serializing/parsing JSON in between.
     */
    static Sink combineSinks(
        std::vector<Sink> sinks
    ) {
        return [
            sinks = std::move(sinks)
        ](
            const CommandEvent& event
        ) mutable {
            for (auto& sink : sinks) {
                if (sink) {
                    sink(event);
                }
            }
        };
    }

private:
    std::string command_;
    Sink sink_;
    std::mutex emit_mutex_;

    void emit(
        const CommandEvent& event
    ) {
        if (!sink_) {
            return;
        }

        /*
         * Preserve event ordering for one operation even when callbacks arrive
         * from multiple threads (for example sync's writer/download threads).
         */
        std::lock_guard<std::mutex> lock(
            emit_mutex_
        );

        sink_(event);
    }

    static std::mutex& stdoutMutex() {
        static std::mutex mutex;
        return mutex;
    }

    static std::string quote(
        const std::string& value
    ) {
        return
            "\"" +
            escape(value) +
            "\"";
    }

    static std::string escape(
        const std::string& value
    ) {
        std::ostringstream out;

        for (
            const unsigned char ch :
            value
        ) {
            switch (ch) {
            case '\\':
                out << "\\\\";
                break;

            case '"':
                out << "\\\"";
                break;

            case '\b':
                out << "\\b";
                break;

            case '\f':
                out << "\\f";
                break;

            case '\n':
                out << "\\n";
                break;

            case '\r':
                out << "\\r";
                break;

            case '\t':
                out << "\\t";
                break;

            default:
                if (ch < 0x20) {
                    static constexpr char HEX[] =
                        "0123456789abcdef";

                    out
                        << "\\u00"
                        << HEX[
                            (ch >> 4) &
                            0x0f
                        ]
                        << HEX[
                            ch &
                            0x0f
                        ];
                }
                else {
                    out
                        << static_cast<char>(
                            ch
                        );
                }

                break;
            }
        }

        return out.str();
    }
};

} // namespace headmotion::app
