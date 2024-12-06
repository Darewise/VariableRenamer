// TODO: Does it work on qualifiers? const? mutable? volatile? restrict? etc...
// RESEARCH: DeclaratorDecl::getQualifier

// TODO: Does it work on default values of function arguments?

#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include <iostream>
#include <regex>

using namespace clang;
using namespace clang::tooling;
using namespace clang::ast_matchers;

static llvm::cl::opt<bool> fromDarewiseToUnreal("to-unreal", llvm::cl::desc("Convert from Darewise to Unreal naming convention"), llvm::cl::init(true));
static llvm::cl::opt<bool> s_verbose("verbose", llvm::cl::desc("Print changes"), llvm::cl::init(true));
static llvm::cl::opt<bool> s_dryRun("dry-run", llvm::cl::desc("Do not overwrite files"), llvm::cl::init(false));
static llvm::cl::opt<std::string> s_excludeRegex("exclude", llvm::cl::desc("Exclude files"), llvm::cl::init(""));

// Helper function to convert camelCase to PascalCase and vice versa
std::string toPascalCase(std::string name)
{
	if (!name.empty())
		name[0] = toupper(name[0]);
	return name;
}

std::string toCamelCase(std::string name)
{
	if (!name.empty())
		name[0] = tolower(name[0]);
	return name;
}

void ReplaceText(Rewriter& a_rewriter, SourceRange& a_sourceRange, const std::string& a_replacement)
{
	if (s_verbose)
	{
		std::string old;
		old = Lexer::getSourceText(CharSourceRange::getTokenRange(a_sourceRange), a_rewriter.getSourceMgr(), a_rewriter.getLangOpts());
		std::cout << old << std::endl << a_replacement << std::endl << std::endl;
	}

	if (s_dryRun)
	{
		return;
	}

	bool couldReplace = !a_rewriter.ReplaceText(a_sourceRange, a_replacement);
	assert(couldReplace);
	bool couldOverwrite = !a_rewriter.overwriteChangedFiles();
	assert(couldOverwrite);
}

class FindMemberVar : public MatchFinder::MatchCallback
{
public:
	FindMemberVar(MatchFinder& a_finder)
	: m_exclude(s_excludeRegex)
	{
		a_finder.addMatcher(fieldDecl().bind("memberVar"), this);
	}

	const std::set<std::string>& getFound() const { return m_found; }

	void run(const MatchFinder::MatchResult &a_result) override {
		const SourceManager& sourceManager = *a_result.SourceManager;

		if (const FieldDecl* fieldDecl = a_result.Nodes.getNodeAs<FieldDecl>("memberVar"))
		{
			const std::string name = fieldDecl->getName().str();
			const std::string filename = std::string(sourceManager.getFilename(fieldDecl->getLocation()));
			if (std::regex_match(filename, m_exclude))
			{
				std::cout << "Excluding: " << name << " in " << filename << std::endl;
				return;
			}
			std::cout << "Found: " << name << " in " << filename << std::endl;
			m_found.insert(name);
		}
	}

private:
	std::set<std::string> m_found;
	const std::regex m_exclude;
};

class ReplaceMemberVar : public MatchFinder::MatchCallback
{
public:
	ReplaceMemberVar(MatchFinder& a_finder, Rewriter& a_rewriter, const std::set<std::string>& a_toReplace)
		: m_rewriter(a_rewriter)
	{
		for (const auto& found : a_toReplace)
		{
			const std::string prefix = "m_";
			if (fromDarewiseToUnreal)
			{
				if (found.rfind(prefix, 0) != 0)
				{
					// Skip if it does not start with prefix
					continue;
				}
				// Remove prefix and convert to pascal
				const std::string replacement = toPascalCase(found.substr(prefix.size()));
				m_replace.insert_or_assign(found, replacement);
			}
			else
			{
				if (found.rfind(prefix, 0) == 0)
				{
					// Skip if it does start with prefix
					continue;
				}
				// Add prefix and convert to camel
				const std::string replacement = prefix + toCamelCase(found);
				m_replace.insert_or_assign(found, replacement);
			}

			a_finder.addMatcher(fieldDecl(hasName(found)).bind("memberVar"), this);
			a_finder.addMatcher(memberExpr(hasDeclaration(fieldDecl(hasName(found)))).bind("memberRef"), this);
		}
	}

	void run(const MatchFinder::MatchResult& a_result) override
	{
		SourceManager& sourceManager = *a_result.SourceManager;
		const LangOptions& langOpts = a_result.Context->getLangOpts();
		m_rewriter.setSourceMgr(sourceManager, langOpts);

		SourceRange sourceRange;
		std::string replacement;

		if (const FieldDecl* fieldDecl = a_result.Nodes.getNodeAs<FieldDecl>("memberVar"))
		{
			const std::string& memberName = fieldDecl->getName().str();
			const auto it = m_replace.find(memberName);
			if (it == m_replace.end())
			{
				return;
			}

			std::string initCode;
			if (const Expr* Init = fieldDecl->getInClassInitializer())
			{
				initCode = Lexer::getSourceText(CharSourceRange::getTokenRange(Init->getSourceRange()), sourceManager, langOpts);
			}

			sourceRange = fieldDecl->getSourceRange();
			replacement = fieldDecl->getType().getAsString() + " " + it->second + (initCode.empty() ? "" : (" = " + initCode));
		}
		else if (const MemberExpr* memberExpr = a_result.Nodes.getNodeAs<MemberExpr>("memberRef"))
		{
			const std::string& memberName = memberExpr->getMemberDecl()->getName().str();
			const auto it = m_replace.find(memberName);
			if (it == m_replace.end())
			{
				return;
			}
			sourceRange = memberExpr->getSourceRange();
			replacement = it->second;
		}
		else
		{
			assert(false);
		}

		ReplaceText(m_rewriter, sourceRange, replacement);
	}

private:
	Rewriter& m_rewriter;
	std::map<std::string, std::string> m_replace;
};

class FindArgVar : public MatchFinder::MatchCallback
{
public:
	FindArgVar(MatchFinder& a_finder)
	: m_exclude(s_excludeRegex)
	{
		a_finder.addMatcher(parmVarDecl().bind("argVar"), this);
	}

	const std::set<std::string>& getFound() const { return m_found; }

	void run(const MatchFinder::MatchResult& a_result) override
	{
		const SourceManager& sourceManager = *a_result.SourceManager;

		if (const ParmVarDecl* parmVarDecl = a_result.Nodes.getNodeAs<ParmVarDecl>("argVar"))
		{
			const std::string name = parmVarDecl->getName().str();
			const std::string filename = std::string(sourceManager.getFilename(parmVarDecl->getLocation()));
			if (std::regex_match(filename, m_exclude))
			{
				std::cout << "Excluding: " << name << " in " << filename << std::endl;
				return;
			}
			std::cout << "Found: " << name << " in " << filename << std::endl;
			m_found.insert(name);
		}
	}

private:
	std::set<std::string> m_found;
	const std::regex m_exclude;
};

class ReplaceArgVar : public MatchFinder::MatchCallback
{
public:
	ReplaceArgVar(MatchFinder& a_finder, Rewriter& a_rewriter, const std::set<std::string>& a_toReplace)
		: m_rewriter(a_rewriter)
	{
		for (const auto& found : a_toReplace)
		{
			const std::string prefix = "a_";
			if (fromDarewiseToUnreal)
			{
				if (found.rfind(prefix, 0) != 0)
				{
					// Skip if it does not start with prefix
					continue;
				}
				// Remove prefix and convert to pascal
				const std::string replacement = toPascalCase(found.substr(prefix.size()));
				m_replace.insert_or_assign(found, replacement);
			}
			else
			{
				if (found.rfind(prefix, 0) == 0)
				{
					// Skip if it does start with prefix
					continue;
				}
				// Add prefix and convert to camel
				const std::string replacement = prefix + toCamelCase(found);
				m_replace.insert_or_assign(found, replacement);
			}

			a_finder.addMatcher(parmVarDecl(hasName(found)).bind("argVar"), this);
			a_finder.addMatcher(declRefExpr(to(parmVarDecl(hasName(found)))).bind("argRef"), this);
		}
	}

	void run(const MatchFinder::MatchResult& a_result) override
	{
		SourceRange sourceRange;
		std::string replacement;

		m_rewriter.setSourceMgr(a_result.Context->getSourceManager(), a_result.Context->getLangOpts());
		if (const ParmVarDecl* parmVarDecl = a_result.Nodes.getNodeAs<ParmVarDecl>("argVar"))
		{
			const std::string& argName = parmVarDecl->getName().str();
			const auto it = m_replace.find(argName);
			if (it == m_replace.end())
			{
				return;
			}

			// TODO: Does it work with default values?

			sourceRange = parmVarDecl->getSourceRange();
			replacement = parmVarDecl->getType().getAsString() + " " + it->second;
		}
		else if (const DeclRefExpr *declRefExpr = a_result.Nodes.getNodeAs<DeclRefExpr>("argRef"))
		{
			const std::string& argName = declRefExpr->getFoundDecl()->getName().str();
			const auto it = m_replace.find(argName);
			if (it == m_replace.end())
			{
				return;
			}

			sourceRange = declRefExpr->getSourceRange();
			replacement = it->second;
		}
		else
		{
			assert(false);
		}

		ReplaceText(m_rewriter, sourceRange, replacement);
	}

private:
	Rewriter& m_rewriter;
	std::map<std::string, std::string> m_replace;
};

class FindLocalVar : public MatchFinder::MatchCallback
{
public:
	FindLocalVar(MatchFinder& a_finder)
	: m_exclude(s_excludeRegex)
	{
		a_finder.addMatcher(varDecl(hasLocalStorage(), unless(parmVarDecl())).bind("localVar"), this);
	}

	const std::set<std::string>& getFound() const { return m_found; }

	void run(const MatchFinder::MatchResult& a_result) override
	{
		const SourceManager& sourceManager = *a_result.SourceManager;

		if (const VarDecl *varDecl = a_result.Nodes.getNodeAs<VarDecl>("localVar"))
		{
			const std::string name = varDecl->getName().str();
			const std::string filename = std::string(sourceManager.getFilename(varDecl->getLocation()));
			if (std::regex_match(filename, m_exclude))
			{
				std::cout << "Excluding: " << name << " in " << filename << std::endl;
				return;
			}
			std::cout << "Found: " << name << " in " << filename << std::endl;
			m_found.insert(name);
		}
	}

private:
	std::set<std::string> m_found;
	const std::regex m_exclude;
};

class ReplaceLocalVar : public MatchFinder::MatchCallback
{
public:
	ReplaceLocalVar(MatchFinder& a_finder, Rewriter& a_rewriter, const std::set<std::string>& a_toReplace)
		: m_rewriter(a_rewriter)
	{
		for (const auto& found : a_toReplace)
		{
			if (fromDarewiseToUnreal)
			{
				const std::string replacement = toPascalCase(found);
				if (replacement != found)
				{
					m_replace.insert_or_assign(found, replacement);
				}
			}
			else
			{
				const std::string replacement = toCamelCase(found);
				if (replacement != found)
				{
					m_replace.insert_or_assign(found, replacement);
				}
			}

			a_finder.addMatcher(varDecl(hasName(found), hasLocalStorage(), unless(parmVarDecl())).bind("localVar"), this);
			a_finder.addMatcher(
				declRefExpr(to(varDecl(hasName(found), hasLocalStorage(), unless(parmVarDecl())))).bind("varRef"), this);
		}
	}

	void run(const MatchFinder::MatchResult& a_result) override
	{
		SourceManager& sourceManager = *a_result.SourceManager;
		const LangOptions& langOpts = a_result.Context->getLangOpts();
		m_rewriter.setSourceMgr(sourceManager, langOpts);

		SourceRange sourceRange;
		std::string replacement;

		if (const VarDecl *varDecl = a_result.Nodes.getNodeAs<VarDecl>("localVar"))
		{
			const std::string& argName = varDecl->getName().str();
			const auto it = m_replace.find(argName);
			if (it == m_replace.end())
			{
				return;
			}

			std::string initCode;
			if (const Expr* Init = varDecl->getAnyInitializer())
			{
				initCode = Lexer::getSourceText(CharSourceRange::getTokenRange(Init->getSourceRange()), sourceManager, langOpts);
			}

			sourceRange = varDecl->getSourceRange();
			replacement = varDecl->getType().getAsString() + " " + it->second + (initCode.empty() ? "" : (" = " + initCode));
		}
		else if (const DeclRefExpr *declRefExpr = a_result.Nodes.getNodeAs<DeclRefExpr>("varRef"))
		{
			const std::string& argName = declRefExpr->getFoundDecl()->getName().str();
			const auto it = m_replace.find(argName);
			if (it == m_replace.end())
			{
				return;
			}

			sourceRange = declRefExpr->getSourceRange();
			replacement = it->second;
		}
		else
		{
			assert(false);
		}

		ReplaceText(m_rewriter, sourceRange, replacement);
	}

private:
	Rewriter& m_rewriter;
	std::map<std::string, std::string> m_replace;
};

class FindStaticVar : public MatchFinder::MatchCallback
{
public:
	FindStaticVar(MatchFinder& a_finder)
	: m_exclude(s_excludeRegex)
	{
		a_finder.addMatcher(varDecl(isStaticStorageClass()).bind("staticVar"), this);
	}

	const std::set<std::string>& getFound() const { return m_found; }

	void run(const MatchFinder::MatchResult& a_result) override
	{
		const SourceManager& sourceManager = *a_result.SourceManager;

		if (const VarDecl* varDecl = a_result.Nodes.getNodeAs<VarDecl>("staticVar"))
		{
			const std::string name = varDecl->getName().str();
			const std::string filename = std::string(sourceManager.getFilename(varDecl->getLocation()));
			if (std::regex_match(filename, m_exclude))
			{
				std::cout << "Excluding: " << name << " in " << filename << std::endl;
				return;
			}
			std::cout << "Found: " << name << " in " << filename << std::endl;
			m_found.insert(name);
		}
	}

private:
	std::set<std::string> m_found;
	const std::regex m_exclude;
};

class ReplaceStaticVar : public MatchFinder::MatchCallback
{
public:
	ReplaceStaticVar(MatchFinder& a_finder, Rewriter& a_rewriter, const std::set<std::string>& a_toReplace)
		: m_rewriter(a_rewriter)
	{
		for (const auto& found : a_toReplace)
		{
			const std::string prefix = "s_";
			if (fromDarewiseToUnreal)
			{
				if (found.rfind(prefix, 0) != 0)
				{
					// Skip if it does not start with prefix
					continue;
				}
				// Remove prefix and convert to pascal
				const std::string replacement = toPascalCase(found.substr(prefix.size()));
				m_replace.insert_or_assign(found, replacement);
			}
			else
			{
				if (found.rfind(prefix, 0) == 0)
				{
					// Skip if it does start with prefix
					continue;
				}
				// Add prefix and convert to camel
				const std::string replacement = prefix + toCamelCase(found);
				m_replace.insert_or_assign(found, replacement);
			}

			a_finder.addMatcher(varDecl(hasName(found), isStaticStorageClass()).bind("staticVar"), this);
			a_finder.addMatcher(declRefExpr(to(varDecl(hasName(found), isStaticStorageClass()))).bind("varRef"), this);
		}
	}

	void run(const MatchFinder::MatchResult& a_result) override
	{
		SourceManager& sourceManager = *a_result.SourceManager;
		const LangOptions& langOpts = a_result.Context->getLangOpts();
		m_rewriter.setSourceMgr(sourceManager, langOpts);

		SourceRange sourceRange;
		std::string replacement;

		if (const VarDecl* varDecl = a_result.Nodes.getNodeAs<VarDecl>("staticVar"))
		{
			const std::string& argName = varDecl->getName().str();
			const auto it = m_replace.find(argName);
			if (it == m_replace.end())
			{
				return;
			}

			std::string initCode;
			if (const Expr* Init = varDecl->getAnyInitializer())
			{
				if (varDecl->isStaticDataMember())
				{
					const VarDecl* definition = varDecl->getDefinition();
					definition = definition ? definition : varDecl->getActingDefinition();
					assert(definition);

					// We need to modify the definition which is at another location:
					// std::cerr << std::string(Lexer::getSourceText(CharSourceRange::getTokenRange(definition->getSourceRange()), sourceManager, langOpts)) << std::endl;

					// Shadow initCode on purpose.
					std::string initCode = std::string(Lexer::getSourceText(CharSourceRange::getTokenRange(Init->getSourceRange()), sourceManager, langOpts));
					std::string qualName;
					llvm::raw_string_ostream os(qualName);
					varDecl->printNestedNameSpecifier(os);
					std::string newDefinition = varDecl->getType().getAsString()
						+ " " + qualName + it->second + (initCode.empty() ? "" : (" = " + initCode));
					ReplaceText(m_rewriter, definition->getSourceRange(), newDefinition);
				}
				else
				{
					initCode = Lexer::getSourceText(CharSourceRange::getTokenRange(Init->getSourceRange()), sourceManager, langOpts);
				}
			}

			sourceRange = varDecl->getSourceRange();
			replacement = "static " + varDecl->getType().getAsString() + " " + it->second + (initCode.empty() ? "" : (" = " + initCode));
		}
		else if (const DeclRefExpr* declRefExpr = a_result.Nodes.getNodeAs<DeclRefExpr>("varRef"))
		{
			const std::string& argName = declRefExpr->getFoundDecl()->getName().str();
			const auto it = m_replace.find(argName);
			if (it == m_replace.end())
			{
				return;
			}

			sourceRange = declRefExpr->getSourceRange();
			replacement = it->second;
		}
		else
		{
			assert(false);
		}

		ReplaceText(m_rewriter, sourceRange, replacement);
	}

private:
	Rewriter& m_rewriter;
	std::map<std::string, std::string> m_replace;
};

int main(int argc, const char **argv) {
	static llvm::cl::OptionCategory MyToolCategory("Naming convention renaming tool");
	auto ExpectedParser = CommonOptionsParser::create(argc, argv, MyToolCategory);
	if (!ExpectedParser)
	{
		llvm::errs() << ExpectedParser.takeError();
		return 1;
	}
	CommonOptionsParser& OptionsParser = ExpectedParser.get();
    ClangTool Tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());

	MatchFinder findOnly;
	FindMemberVar findMemberVar {findOnly};
	FindArgVar findArgVar {findOnly};
	FindLocalVar findLocalVar {findOnly};
	FindStaticVar findStaticVar {findOnly};

	bool canRunFindOnly = 0 == Tool.run(newFrontendActionFactory(&findOnly).get());
	assert(canRunFindOnly);

	MatchFinder findAndReplace;
	Rewriter rewriter;
	ReplaceMemberVar replaceMemberVar {findAndReplace, rewriter, findMemberVar.getFound()};
	ReplaceArgVar replaceArgVar {findAndReplace, rewriter, findArgVar.getFound()};
	ReplaceLocalVar replaceLocalVar {findAndReplace, rewriter, findLocalVar.getFound()};
	ReplaceStaticVar replaceStaticVar {findAndReplace, rewriter, findStaticVar.getFound()};
	
	bool canRunFindAndReplace = 0 == Tool.run(newFrontendActionFactory(&findAndReplace).get());
	assert(canRunFindAndReplace);
}
