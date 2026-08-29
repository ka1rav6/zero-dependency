// core/completions.cpp
//
// The generated shell completion scripts (design doc Milestone 10).

#include "core/completions.hpp"

#include <algorithm>

namespace kap
{
namespace completions
{

namespace
{

// Space-joined, for embedding in a shell word list.
std::string joined(const std::vector<std::string>& words)
{
    std::string out;
    for (const std::string& word : words) {
        if (!out.empty())
            out += ' ';
        out += word;
    }
    return out;
}

// The global flags, in one place so all three scripts agree.
const char* kGlobalFlags = "--dry-run -n --verbose --root --set --help -h --version -V";

std::string bash_script(const std::string& commands, const std::string& plugin_subcommands)
{
    return R"(# bash completion for kap. Install with:
#     kap completions bash > /etc/bash_completion.d/kap
# or, without root:
#     kap completions bash > ~/.local/share/bash-completion/completions/kap

_kap() {
    local cur prev words cword
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    # --root takes a directory; hand that back to bash's own completer rather
    # than trying to reimplement path completion here.
    if [ "$prev" = "--root" ]; then
        COMPREPLY=( $(compgen -d -- "$cur") )
        return
    fi
    # --set takes key=value, which only the matched plugin's schema knows.
    if [ "$prev" = "--set" ]; then
        COMPREPLY=()
        return
    fi

    # Find the first non-flag word: that is the command.
    local i command=""
    for (( i=1; i < COMP_CWORD; i++ )); do
        case "${COMP_WORDS[i]}" in
            -*) ;;
            *) command="${COMP_WORDS[i]}"; break ;;
        esac
    done

    if [ -z "$command" ]; then
        COMPREPLY=( $(compgen -W ")" +
           commands + R"( config detect plugin completions )" + std::string(kGlobalFlags) +
           R"(" -- "$cur") )
        return
    fi

    case "$command" in
        plugin)
            COMPREPLY=( $(compgen -W ")" +
           plugin_subcommands + R"(" -- "$cur") )
            ;;
        config)
            COMPREPLY=( $(compgen -W "get set edit --global --project" -- "$cur") )
            ;;
        completions)
            COMPREPLY=( $(compgen -W "bash zsh fish" -- "$cur") )
            ;;
        detect)
            COMPREPLY=( $(compgen -W "--refresh" -- "$cur") )
            ;;
        dev)
            COMPREPLY=( $(compgen -W "-o --open )" +
           std::string(kGlobalFlags) + R"(" -- "$cur") )
            ;;
        *)
            COMPREPLY=( $(compgen -W ")" +
           std::string(kGlobalFlags) + R"(" -- "$cur") )
            ;;
    esac
}

complete -F _kap kap
)";
}

std::string zsh_script(const std::string& commands, const std::string& plugin_subcommands)
{
    return R"(#compdef kap
# zsh completion for kap. Install with:
#     kap completions zsh > "${fpath[1]}/_kap"
# then restart the shell (or run `compinit`).

_kap() {
    local -a commands plugin_subcommands
    commands=()" +
           commands + R"( config detect plugin completions)
    plugin_subcommands=()" +
           plugin_subcommands + R"()

    _arguments -C \
        '(-n --dry-run)'{-n,--dry-run}'[print the commands instead of running them]' \
        '--verbose[explain what kap is doing]' \
        '--root[search root for the project]:directory:_files -/' \
        '*--set[override one plugin config key]:key=value:' \
        '(-h --help)'{-h,--help}'[show usage]' \
        '(-V --version)'{-V,--version}'[show the version]' \
        '1: :->command' \
        '*:: :->argument'

    case $state in
        command)
            _describe 'kap command' commands
            ;;
        argument)
            case $words[1] in
                plugin)
                    _describe 'plugin subcommand' plugin_subcommands
                    ;;
                config)
                    _values 'config subcommand' get set edit
                    ;;
                completions)
                    _values 'shell' bash zsh fish
                    ;;
                detect)
                    _arguments '--refresh[ignore the cached resolution]'
                    ;;
                dev)
                    _arguments '(-o --open)'{-o,--open}'[open the first URL a step prints]'
                    ;;
            esac
            ;;
    esac
}

_kap "$@"
)";
}

std::string fish_script(const std::vector<std::string>& project_commands,
                        const std::vector<std::string>& plugin_subcommands)
{
    std::string out = R"(# fish completion for kap. Install with:
#     kap completions fish > ~/.config/fish/completions/kap.fish

# Only offer commands until one has been given.
function __kap_no_command
    for word in (commandline -opc)[2..-1]
        if not string match -q -- '-*' $word
            return 1
        end
    end
    return 0
end

complete -c kap -f

# Global flags.
complete -c kap -s n -l dry-run -d 'print the commands instead of running them'
complete -c kap      -l verbose -d 'explain what kap is doing'
complete -c kap      -l root -r -a '(__fish_complete_directories)' -d 'search root for the project'
complete -c kap      -l set -x -d 'override one plugin config key (key=value)'
complete -c kap -s h -l help -d 'show usage'
complete -c kap -s V -l version -d 'show the version'

# Project commands.
)";
    for (const std::string& command : project_commands)
        out += "complete -c kap -n __kap_no_command -a " + command + "\n";

    out += R"(
# kap's own commands.
complete -c kap -n __kap_no_command -a detect      -d 'show which plugin claims this directory'
complete -c kap -n __kap_no_command -a config      -d 'read or write configuration'
complete -c kap -n __kap_no_command -a plugin      -d 'manage plugins'
complete -c kap -n __kap_no_command -a completions -d 'print a shell completion script'

# Subcommands.
)";
    for (const std::string& subcommand : plugin_subcommands)
        out += "complete -c kap -n '__fish_seen_subcommand_from plugin' -a " + subcommand + "\n";

    out += R"(complete -c kap -n '__fish_seen_subcommand_from config' -a 'get set edit'
complete -c kap -n '__fish_seen_subcommand_from completions' -a 'bash zsh fish'
complete -c kap -n '__fish_seen_subcommand_from detect' -l refresh -d 'ignore the cached resolution'
complete -c kap -n '__fish_seen_subcommand_from dev' -s o -l open -d 'open the first URL a step prints'
)";
    return out;
}

} // namespace

std::vector<std::string> shells()
{
    return {"bash", "zsh", "fish"};
}

std::string script(std::string_view                shell,
                   const std::vector<std::string>& project_commands,
                   const std::vector<std::string>& plugin_subcommands)
{
    const std::string commands = joined(project_commands);
    const std::string plugins  = joined(plugin_subcommands);

    if (shell == "bash")
        return bash_script(commands, plugins);
    if (shell == "zsh")
        return zsh_script(commands, plugins);
    if (shell == "fish")
        return fish_script(project_commands, plugin_subcommands);
    return {};
}

} // namespace completions
} // namespace kap
