#include <nix/config.h>

#include <cstdlib>
#include <iostream>

#include <setjmp.h>

#include <readline/history.h>
#include <readline/readline.h>

#include "common-eval-args.hh"
#include "derivations.hh"
#include "eval-inline.hh"
#include "eval.hh"
#include "get-drvs.hh"
#include "globals.hh"
#include "local-fs-store.hh"
#include "shared.hh"
#include "store-api.hh"

#if HAVE_BOEHMGC
#define GC_INCLUDE_NEW
#include <gc/gc_cpp.h>
#endif

using namespace std;
using namespace nix;

#define ESC_RED "\033[31m"
#define ESC_GRE "\033[32m"
#define ESC_YEL "\033[33m"
#define ESC_BLU "\033[34;1m"
#define ESC_MAG "\033[35m"
#define ESC_CYA "\033[36m"
#define ESC_END "\033[0m"

string programId = "nix-repl";
const string historyFile = string(getenv("HOME")) + "/.nix-repl-history";

struct NixRepl
#if HAVE_BOEHMGC
    : gc
#endif
{
  string curDir;
  std::unique_ptr<EvalState> state;

  Strings loadedFiles;

  const static int envSize = 32768;
  std::shared_ptr<StaticEnv> staticEnv;
  Env *env;
  int displ;
  StringSet varNames;

  StringSet completions;
  StringSet::iterator curCompletion;

  NixRepl(const Strings &searchPath, nix::ref<Store> store);
  void mainLoop(const Strings &files);
  void completePrefix(string prefix);
  bool getLine(string &input, const char *prompt);
  StorePath getDerivationPath(Value &v);
  bool processLine(string line);
  void loadFile(const Path &path);
  void initEnv();
  void reloadFiles();
  void addAttrsToScope(Value &attrs);
  void addVarToScope(const Symbol &name, Value &v);
  Expr *parseString(string s);
  void evalString(string s, Value &v);

  typedef set<Value *> ValuesSeen;
  std::ostream &printValue(std::ostream &str, Value &v, unsigned int maxDepth);
  std::ostream &printValue(std::ostream &str, Value &v, unsigned int maxDepth,
                           ValuesSeen &seen);
};

void printHelp() {
  cout << "Usage: nix-repl [--help] [--version] [-I path] paths...\n"
       << "\n"
       << "nix-repl is a simple read-eval-print loop (REPL) for the Nix "
          "package manager.\n"
       << "\n"
       << "Options:\n"
       << "    --help\n"
       << "        Prints out a summary of the command syntax and exits.\n"
       << "\n"
       << "    --version\n"
       << "        Prints out the Nix version number on standard output and "
          "exits.\n"
       << "\n"
       << "    -I path\n"
       << "        Add a path to the Nix expression search path. This option "
          "may be given\n"
       << "        multiple times. See the NIX_PATH environment variable for "
          "information on\n"
       << "        the semantics of the Nix search path. Paths added through "
          "-I take\n"
       << "        precedence over NIX_PATH.\n"
       << "\n"
       << "    paths...\n"
       << "        A list of paths to files containing Nix expressions which "
          "nix-repl will\n"
       << "        load and add to its scope.\n"
       << "\n"
       << "        A path surrounded in < and > will be looked up in the Nix "
          "expression search\n"
       << "        path, as in the Nix language itself.\n"
       << "\n"
       << "        If an element of paths starts with http:// or https://, it "
          "is interpreted\n"
       << "        as the URL of a tarball that will be downloaded and "
          "unpacked to a temporary\n"
       << "        location. The tarball must include a single top-level "
          "directory containing\n"
       << "        at least a file named default.nix.\n"
       << flush;
}

string removeWhitespace(string s) {
  s = chomp(s);
  size_t n = s.find_first_not_of(" \n\r\t");
  if (n != string::npos)
    s = string(s, n);
  return s;
}

NixRepl::NixRepl(const Strings &searchPath, nix::ref<Store> store)
    : state(std::make_unique<EvalState>(searchPath, store)),
      staticEnv(new StaticEnv(false, state->staticBaseEnv.get())) {
  curDir = absPath(".");
}

void NixRepl::mainLoop(const Strings &files) {
  string error = ANSI_RED "error:" ANSI_NORMAL " ";
  std::cout << "Welcome to Nix version " << NIX_VERSION << ". Type :? for help."
            << std::endl
            << std::endl;

  for (auto &i : files)
    loadedFiles.push_back(i);

  reloadFiles();
  if (!loadedFiles.empty())
    std::cout << std::endl;

  // Allow nix-repl specific settings in .inputrc
  rl_readline_name = "nix-repl";
  using_history();
  read_history(historyFile.c_str());

  string input;

  while (true) {
    // When continuing input from previous lines, don't print a prompt, just
    // align to the same number of chars as the prompt.
    const char *prompt = input.empty() ? "nix-repl> " : "          ";
    if (!getLine(input, prompt)) {
      std::cout << std::endl;
      break;
    }

    try {
      if (!removeWhitespace(input).empty() && !processLine(input))
        return;
    } catch (ParseError &e) {
      if (e.msg().find("unexpected $end") != std::string::npos) {
        // For parse errors on incomplete input, we continue waiting for the
        // next line of input without clearing the input so far.
        continue;
      } else {
        printMsg(lvlError, e.msg());
      }
    } catch (Error &e) {
      printMsg(lvlError, e.msg());
    } catch (Interrupted &e) {
      printMsg(lvlError, e.msg());
    }

    // We handled the current input fully, so we should clear it and read brand
    // new input.
    input.clear();
    std::cout << std::endl;
  }
}

/* Apparently, the only way to get readline() to return on Ctrl-C
   (SIGINT) is to use siglongjmp().  That's fucked up... */
static sigjmp_buf sigintJmpBuf;

static void sigintHandler(int signo) { siglongjmp(sigintJmpBuf, 1); }

/* Oh, if only g++ had nested functions... */
NixRepl *curRepl;

char *completerThunk(const char *s, int state) {
  string prefix(s);

  /* If the prefix has a slash in it, use readline's builtin filename
     completer. */
  if (prefix.find('/') != string::npos)
    return rl_filename_completion_function(s, state);

  /* Otherwise, return all symbols that start with the prefix. */
  if (state == 0) {
    curRepl->completePrefix(s);
    curRepl->curCompletion = curRepl->completions.begin();
  }
  if (curRepl->curCompletion == curRepl->completions.end())
    return 0;
  return strdup((curRepl->curCompletion++)->c_str());
}

bool NixRepl::getLine(string &input, const char *prompt) {
  struct sigaction act, old;
  act.sa_handler = sigintHandler;
  sigfillset(&act.sa_mask);
  act.sa_flags = 0;
  if (sigaction(SIGINT, &act, &old))
    throw SysError("installing handler for SIGINT");

  if (sigsetjmp(sigintJmpBuf, 1)) {
    input.clear();
  } else {
    curRepl = this;
    rl_completion_entry_function = completerThunk;

    char *s = readline(prompt);
    if (!s)
      return false;
    input.append(s);
    input.push_back('\n');
    if (!removeWhitespace(s).empty()) {
      add_history(s);
      append_history(1, 0);
    }
    free(s);
  }

  _isInterrupted = 0;

  if (sigaction(SIGINT, &old, 0))
    throw SysError("restoring handler for SIGINT");

  return true;
}

void NixRepl::completePrefix(string prefix) {
  completions.clear();

  size_t dot = prefix.rfind('.');

  if (dot == string::npos) {
    /* This is a variable name; look it up in the current scope. */
    StringSet::iterator i = varNames.lower_bound(prefix);
    while (i != varNames.end()) {
      if (string(*i, 0, prefix.size()) != prefix)
        break;
      completions.insert(*i);
      i++;
    }
  } else {
    try {
      /* This is an expression that should evaluate to an
         attribute set.  Evaluate it to get the names of the
         attributes. */
      string expr(prefix, 0, dot);
      string prefix2 = string(prefix, dot + 1);

      Expr *e = parseString(expr);
      Value v;
      e->eval(*state, *env, v);
      state->forceAttrs(v, noPos);

      for (auto &i : *v.attrs) {
        string_view name = state->symbols[i.name];
        if (string(name, 0, prefix2.size()) != prefix2)
          continue;
        completions.insert(expr + "." + name);
      }

    } catch (ParseError &e) {
      // Quietly ignore parse errors.
    } catch (EvalError &e) {
      // Quietly ignore evaluation errors.
    } catch (UndefinedVarError &e) {
      // Quietly ignore undefined variable errors.
    }
  }
}

void runNix(Path program, const Strings &args,
            const std::optional<std::string> &input = {}) {
  auto subprocessEnv = getEnv();
  subprocessEnv["NIX_CONFIG"] = globalConfig.toKeyValue();

  runProgram2(RunOptions{
      .program = settings.nixBinDir + "/" + program,
      .args = args,
      .environment = subprocessEnv,
      .input = input,
  });

  return;
}

bool isVarName(const string &s) {
  if (s.size() == 0)
    return false;
  char c = s[0];
  if ((c >= '0' && c <= '9') || c == '-' || c == '\'')
    return false;
  for (auto &i : s)
    if (!((i >= 'a' && i <= 'z') || (i >= 'A' && i <= 'Z') ||
          (i >= '0' && i <= '9') || i == '_' || i == '-' || i == '\''))
      return false;
  return true;
}

StorePath NixRepl::getDerivationPath(Value &v) {
  auto drvInfo = getDerivation(*state, v, false);
  if (!drvInfo)
    throw Error(
        "expression does not evaluate to a derivation, so I can't build it");
  auto drvPath = drvInfo->queryDrvPath();
  if (!drvPath)
    throw Error("expression did not evaluate to a valid derivation (no "
                "'drvPath' attribute)");
  if (!state->store->isValidPath(*drvPath))
    throw Error("expression evaluated to invalid derivation '%s'",
                state->store->printStorePath(*drvPath));
  return *drvPath;
}

bool NixRepl::processLine(string line) {
  if (line == "")
    return true;

  string command, arg;

  if (line[0] == ':') {
    size_t p = line.find_first_of(" \n\r\t");
    command = string(line, 0, p);
    if (p != string::npos)
      arg = removeWhitespace(string(line, p));
  } else {
    arg = line;
  }

  if (command == ":?" || command == ":help") {
    cout << "The following commands are available:\n"
         << "\n"
         << "  <expr>        Evaluate and print expression\n"
         << "  <x> = <expr>  Bind expression to variable\n"
         << "  :a <expr>     Add attributes from resulting set to scope\n"
         << "  :b <expr>     Build derivation\n"
         << "  :i <expr>     Build derivation, then install result into "
            "current profile\n"
         << "  :l <path>     Load Nix expression and add it to scope\n"
         << "  :p <expr>     Evaluate and print expression recursively\n"
         << "  :q            Exit nix-repl\n"
         << "  :r            Reload all files\n"
         << "  :s <expr>     Build dependencies of derivation, then start "
            "nix-shell\n"
         << "  :t <expr>     Describe result of evaluation\n"
         << "  :u <expr>     Build derivation, then start nix-shell\n";
  }

  else if (command == ":a" || command == ":add") {
    Value v;
    evalString(arg, v);
    addAttrsToScope(v);
  }

  else if (command == ":l" || command == ":load") {
    state->resetFileCache();
    loadFile(arg);
  }

  else if (command == ":r" || command == ":reload") {
    state->resetFileCache();
    reloadFiles();
  }

  else if (command == ":t") {
    Value v;
    evalString(arg, v);
    std::cout << showType(v) << std::endl;

  } else if (command == ":u") {
    Value v, f, result;
    evalString(arg, v);
    evalString("drv: (import <nixpkgs> {}).runCommand \"shell\" { buildInputs "
               "= [ drv ]; } \"\"",
               f);
    state->callFunction(f, v, result, PosIdx());

    StorePath drvPath = getDerivationPath(result);
    runNix("nix-shell", Strings{state->store->printStorePath(drvPath)});
  }

  else if (command == ":b" || command == ":i" || command == ":s") {
    Value v;
    evalString(arg, v);
    StorePath drvPath = getDerivationPath(v);
    Path drvPathRaw = state->store->printStorePath(drvPath);

    if (command == ":b") {
      state->store->buildPaths({DerivedPath::Built{drvPath}});
      auto drv = state->store->readDerivation(drvPath);
      logger->cout("\nThis derivation produced the following outputs:");
      for (auto &[outputName, outputPath] :
           state->store->queryDerivationOutputMap(drvPath)) {
        auto localStore = state->store.dynamic_pointer_cast<LocalFSStore>();
        if (localStore && command == ":bl") {
          std::string symlink = "repl-result-" + outputName;
          localStore->addPermRoot(outputPath, absPath(symlink));
          logger->cout("  ./%s -> %s", symlink,
                       state->store->printStorePath(outputPath));
        } else {
          logger->cout("  %s -> %s", outputName,
                       state->store->printStorePath(outputPath));
        }
      }
    } else if (command == ":i") {
      runNix("nix-env", {"-i", drvPathRaw});
    } else {
      runNix("nix-shell", {drvPathRaw});
    }
  }

  else if (command == ":p" || command == ":print") {
    Value v;
    evalString(arg, v);
    printValue(std::cout, v, 1000000000) << std::endl;
  }

  else if (command == ":q" || command == ":quit")
    return false;

  else if (command != "")
    throw Error("unknown command ‘%1%’", command);

  else {
    size_t p = line.find('=');
    string name;
    if (p != string::npos && p < line.size() && line[p + 1] != '=' &&
        isVarName(name = removeWhitespace(string(line, 0, p)))) {
      Expr *e = parseString(string(line, p + 1));
      Value &v(*state->allocValue());
      v.mkThunk(env, e);
      addVarToScope(state->symbols.create(name), v);
    } else {
      Value v;
      evalString(line, v);
      printValue(std::cout, v, 1) << std::endl;
    }
  }

  return true;
}

void NixRepl::loadFile(const Path &path) {
  loadedFiles.remove(path);
  loadedFiles.push_back(path);
  Value v, v2;
  state->evalFile(lookupFileArg(*state, path), v);
  Bindings &bindings(*state->allocBindings(0));
  state->autoCallFunction(bindings, v, v2);
  addAttrsToScope(v2);
}

void NixRepl::initEnv() {
  env = &state->allocEnv(envSize);
  env->up = &state->baseEnv;
  displ = 0;
  staticEnv->vars.clear();

  varNames.clear();
  for (auto &i : state->staticBaseEnv->vars)
    varNames.emplace(state->symbols[i.first]);
}

void NixRepl::reloadFiles() {
  initEnv();

  Strings old = loadedFiles;
  loadedFiles.clear();

  bool first = true;
  for (auto &i : old) {
    if (!first)
      std::cout << std::endl;
    first = false;
    std::cout << format("Loading ‘%1%’...") % i << std::endl;
    loadFile(i);
  }
}

void NixRepl::addAttrsToScope(Value &attrs) {
  state->forceAttrs(attrs, [&]() { return attrs.determinePos(noPos); });
  for (auto &i : *attrs.attrs)
    addVarToScope(i.name, *i.value);
  std::cout << format("Added %1% variables.") % attrs.attrs->size()
            << std::endl;
}

void NixRepl::addVarToScope(const Symbol &name, Value &v) {
  if (displ >= envSize)
    throw Error("environment full; cannot add more variables");
  staticEnv->vars.emplace_back(name, displ);
  staticEnv->sort();
  env->values[displ++] = &v;
  varNames.insert(state->symbols[name]);
}

Expr *NixRepl::parseString(string s) {
  Expr *e = state->parseExprFromString(s, curDir, staticEnv);
  return e;
}

void NixRepl::evalString(string s, Value &v) {
  Expr *e = parseString(s);
  e->eval(*state, *env, v);
  state->forceValue(v, [&]() { return v.determinePos(noPos); });
}

std::ostream &NixRepl::printValue(std::ostream &str, Value &v,
                                  unsigned int maxDepth) {
  ValuesSeen seen;
  return printValue(str, v, maxDepth, seen);
}

std::ostream &printStringValue(std::ostream &str, const char *string) {
  str << "\"";
  for (const char *i = string; *i; i++)
    if (*i == '\"' || *i == '\\')
      str << "\\" << *i;
    else if (*i == '\n')
      str << "\\n";
    else if (*i == '\r')
      str << "\\r";
    else if (*i == '\t')
      str << "\\t";
    else
      str << *i;
  str << "\"";
  return str;
}

// FIXME: lot of cut&paste from Nix's eval.cc.
std::ostream &NixRepl::printValue(std::ostream &str, Value &v,
                                  unsigned int maxDepth, ValuesSeen &seen) {
  str.flush();
  checkInterrupt();

  state->forceValue(v, [&]() { return v.determinePos(noPos); });

  switch (v.type()) {

  case nInt:
    str << ESC_CYA << v.integer << ESC_END;
    break;

  case nBool:
    str << ESC_CYA << (v.boolean ? "true" : "false") << ESC_END;
    break;

  case nString:
    str << ESC_YEL;
    printStringValue(str, v.string.s);
    str << ESC_END;
    break;

  case nPath:
    str << ESC_GRE << v.path << ESC_END; // !!! escaping?
    break;

  case nNull:
    str << ESC_CYA "null" ESC_END;
    break;

  case nAttrs: {
    seen.insert(&v);

    bool isDrv = state->isDerivation(v);

    if (isDrv) {
      str << "«derivation ";
      Bindings::iterator i = v.attrs->find(state->sDrvPath);
      PathSet context;
      Path drvPath = i != v.attrs->end()
                         ? state->coerceToPath(i->pos, *i->value, context)
                         : "???";
      str << drvPath << "»";
    }

    else if (maxDepth > 0) {
      str << "{ ";

      typedef std::map<string, Value *> Sorted;
      Sorted sorted;
      for (auto &i : *v.attrs)
        sorted.emplace(state->symbols[i.name], i.value);

      for (auto &i : sorted) {
        if (isVarName(i.first))
          str << i.first;
        else
          printStringValue(str, i.first.c_str());
        str << " = ";
        if (seen.find(i.second) != seen.end())
          str << "«repeated»";
        else
          try {
            printValue(str, *i.second, maxDepth - 1, seen);
          } catch (AssertionError &e) {
            str << ESC_RED "«error: " << e.msg() << "»" ESC_END;
          }
        str << "; ";
      }

      str << "}";
    } else
      str << "{ ... }";

    break;
  }

  case nList:
    seen.insert(&v);

    str << "[ ";
    if (maxDepth > 0)
      for (unsigned int n = 0; n < v.listSize(); ++n) {
        if (seen.find(v.listElems()[n]) != seen.end())
          str << "«repeated»";
        else
          try {
            printValue(str, *v.listElems()[n], maxDepth - 1, seen);
          } catch (AssertionError &e) {
            str << ESC_RED "«error: " << e.msg() << "»" ESC_END;
          }
        str << " ";
      }
    else
      str << "... ";
    str << "]";
    break;

  case nFunction:
    if (v.isLambda()) {
      std::ostringstream s;
      s << state->positions[v.lambda.fun->pos];
      str << ANSI_BLUE "«lambda @ " << filterANSIEscapes(s.str())
          << "»" ANSI_NORMAL;
    } else if (v.isPrimOp()) {
      str << ANSI_MAGENTA "«primop»" ANSI_NORMAL;
    } else if (v.isPrimOpApp()) {
      str << ANSI_BLUE "«primop-app»" ANSI_NORMAL;
    } else {
      abort();
    }
    break;

  default:
    str << ESC_RED "«unknown»" ESC_END;
    break;
  }

  return str;
}

int main(int argc, char **argv) {
  return handleExceptions(argv[0], [&]() {
    initNix();
    initGC();

    Strings files, searchPath;

    parseCmdLine(argc, argv,
                 [&](Strings::iterator &arg, const Strings::iterator &end) {
                   if (*arg == "--version")
                     printVersion("nix-repl");
                   else if (*arg == "--help") {
                     printHelp();
                     // exit with 0 since user asked for help
                     _exit(0);
                   }
                   //  else if (parseSearchPathArg(arg, end, searchPath));
                   else if (*arg != "" && arg->at(0) == '-')
                     return false;
                   else
                     files.push_back(*arg);
                   return true;
                 });

    auto repl = make_unique<NixRepl>(searchPath, openStore());
    repl->mainLoop(files);

    write_history(historyFile.c_str());
  });
}
