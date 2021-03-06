/**
 * This source code is licensed under the terms found in the LICENSE file in 
 * node-jsc's root directory.
 */

#include "config.h"
#include "JSCStackTrace.h"

#include <JavaScriptCore/CatchScope.h>
#include <JavaScriptCore/DebuggerPrimitives.h>
#include <JavaScriptCore/Exception.h>
#include <JavaScriptCore/JSCInlines.h>

namespace v8 { namespace jscshim
{

JSCStackTrace JSCStackTrace::fromExisting(JSC::VM& vm, const WTF::Vector<JSC::StackFrame>& existingFrames)
{
	WTF::Vector<JSCStackFrame> newFrames;
	
	size_t frameCount = existingFrames.size();
	if (0 == frameCount)
	{
		return JSCStackTrace();
	}
	
	newFrames.reserveInitialCapacity(frameCount);
	for (size_t i = 0; i < frameCount; i++)
	{
		newFrames.constructAndAppend(vm, existingFrames.at(i));
	}

	return JSCStackTrace(newFrames);
}

JSCStackTrace JSCStackTrace::captureCurrentJSStackTrace(JSC::ExecState * exec, size_t frameLimit, size_t framesToSkip)
{
	JSC::VM& vm = exec->vm();
	JSC::CallFrame * callFrame = vm.topCallFrame;
	if (!callFrame)
	{
		return JSCStackTrace();
	}
	
	WTF::Vector<JSCStackFrame> stackFrames;
	size_t framesCount = 0;

	JSC::DisallowGC disallowGC;

	// Count non-native frames
	size_t framesLeftToSkip = framesToSkip;
	JSC::StackVisitor::visit(callFrame, &vm, [&](JSC::StackVisitor& visitor) -> JSC::StackVisitor::Status {
		if (!visitor->isNativeFrame())
		{
			if (framesLeftToSkip > 0)
			{
				framesLeftToSkip--;
			}
			else
			{
				framesCount++;
			}
		}

		return JSC::StackVisitor::Continue;
	});
	framesCount = std::min(frameLimit, framesCount);

	// Create the actual stack frames
	size_t i = 0;
	framesLeftToSkip = framesToSkip;
	stackFrames.reserveInitialCapacity(framesCount);
	JSC::StackVisitor::visit(callFrame, &vm, [&](JSC::StackVisitor& visitor) -> JSC::StackVisitor::Status {
		// Skip native frames
		if (visitor->isNativeFrame())
		{
			return JSC::StackVisitor::Continue;
		}

		// Skip frames if needed
		if (framesLeftToSkip > 0)
		{
			framesLeftToSkip--;
			return JSC::StackVisitor::Continue;
		}

		stackFrames.constructAndAppend(vm, visitor);
		i++;

		return (i == framesCount) ? JSC::StackVisitor::Done : JSC::StackVisitor::Continue;
	});

	return JSCStackTrace(stackFrames);
}

JSCStackTrace JSCStackTrace::getStackTraceForThrownValue(JSC::VM& vm, JSC::JSValue thrownValue)
{
	const WTF::Vector<JSC::StackFrame> * jscStackTrace = nullptr;

	JSC::Exception * currentException = DECLARE_CATCH_SCOPE(vm).exception();
	if (currentException && currentException->value() == thrownValue)
	{
		jscStackTrace = &currentException->stack();
	}
	else
	{
		JSC::ErrorInstance * error = JSC::jsDynamicCast<JSC::ErrorInstance *>(vm, thrownValue);
		if (error)
		{
			jscStackTrace = error->stackTrace();
		}
	}

	if (!jscStackTrace)
	{
		return JSCStackTrace();
	}
	
	return fromExisting(vm, *jscStackTrace);
}

JSCStackFrame::JSCStackFrame(JSC::VM& vm, JSC::StackVisitor& visitor) :
	m_vm(vm),
	m_codeBlock(nullptr),
	m_bytecodeOffset(UINT_MAX),
	m_sourceURL(nullptr),
	m_functionName(nullptr),
	m_isWasmFrame(false),
	m_sourcePositionsState(SourcePositionsState::NotCalculated)
{
	m_callee = visitor->callee().asCell();
	m_callFrame = visitor->callFrame();

	// Based on JSC's GetStackTraceFunctor (Interpreter.cpp)
	if (visitor->isWasmFrame())
	{
		m_wasmFunctionIndexOrName = visitor->wasmFunctionIndexOrName();
		m_isWasmFrame = true;
	}
	else if (!!visitor->codeBlock() && !visitor->codeBlock()->unlinkedCodeBlock()->isBuiltinFunction())
	{
		m_codeBlock = visitor->codeBlock();
		m_bytecodeOffset = visitor->bytecodeOffset();
	}
}

JSCStackFrame::JSCStackFrame(JSC::VM& vm, const JSC::StackFrame& frame) : 
	m_vm(vm),
	m_callFrame(nullptr),
	m_codeBlock(nullptr),
	m_bytecodeOffset(UINT_MAX),
	m_sourceURL(nullptr),
	m_functionName(nullptr),
	m_isWasmFrame(false),
	m_sourcePositionsState(SourcePositionsState::NotCalculated)
{
	m_callee = frame.callee();

	// Based on JSC's GetStackTraceFunctor (Interpreter.cpp)
	if (frame.isWasmFrame())
	{
		m_wasmFunctionIndexOrName = frame.wasmFunctionIndexOrName();
		m_isWasmFrame = true;
	}
	else
	{
		m_codeBlock = frame.codeBlock();
		if (frame.hasBytecodeOffset())
		{
			m_bytecodeOffset = frame.bytecodeOffset();
		}
	}
}

intptr_t JSCStackFrame::sourceID() const
{
	return m_codeBlock ? m_codeBlock->ownerScriptExecutable()->sourceID() : JSC::noSourceID;
}

JSC::JSString * JSCStackFrame::sourceURL()
{
	if (!m_sourceURL)
	{
		m_sourceURL = retrieveSourceURL();
	}

	return m_sourceURL;
}

JSC::JSString * JSCStackFrame::functionName()
{
	if (!m_functionName)
	{
		m_functionName = retrieveFunctionName();
	}

	return m_functionName;
}

JSCStackFrame::SourcePositions * JSCStackFrame::getSourcePositions()
{
	if (SourcePositionsState::NotCalculated == m_sourcePositionsState)
	{
		m_sourcePositionsState = calculateSourcePositions() ? SourcePositionsState::Calculated : SourcePositionsState::Failed;
	}

	return (SourcePositionsState::Calculated == m_sourcePositionsState) ? &m_sourcePositions : nullptr;
}

/* Based on JSC::StackFrame::sourceURL
 * TODO: Cache the literal strings? */
ALWAYS_INLINE JSC::JSString * JSCStackFrame::retrieveSourceURL()
{
	if (m_isWasmFrame)
	{
		return JSC::jsString(&m_vm, ASCIILiteral("[wasm code]"));
	}

	if (!m_codeBlock)
	{
		return JSC::jsString(&m_vm, ASCIILiteral("[native code]"));
	}

	String sourceURL = m_codeBlock->ownerScriptExecutable()->sourceURL();
	return sourceURL.isNull() ? m_vm.smallStrings.emptyString() : JSC::jsString(&m_vm, sourceURL);
}

ALWAYS_INLINE JSC::JSString * JSCStackFrame::retrieveFunctionName()
{
	if (m_isWasmFrame)
	{
		return JSC::jsString(&m_vm, JSC::Wasm::makeString(m_wasmFunctionIndexOrName));
	}

	if (m_codeBlock)
	{
		// TODO: Use atomic strings\cache the strings?
		switch (m_codeBlock->codeType())
		{
		case JSC::EvalCode:
			return JSC::jsString(&m_vm, ASCIILiteral("eval code"));
		case JSC::ModuleCode:
			return JSC::jsString(&m_vm, ASCIILiteral("module code"));
		case JSC::FunctionCode:
			break;
		case JSC::GlobalCode:
			return JSC::jsString(&m_vm, ASCIILiteral("global code"));
		default:
			ASSERT_NOT_REACHED();
		}
	}

	if (!m_callee || !m_callee->isObject())
	{
		return m_vm.smallStrings.emptyString();
	}

	JSC::JSObject * calleeAsObject = JSC::jsCast<JSC::JSObject *>(m_callee);

	// First, try the "displayName" property
	JSC::JSValue displayName = calleeAsObject->getDirect(m_vm, m_vm.propertyNames->displayName);
	if (displayName && isJSString(displayName))
	{
		return JSC::asString(displayName);
	}

	// Our addition - if there's no "dispalyName" property, try the "name" property
	JSC::JSValue name = calleeAsObject->getDirect(m_vm, m_vm.propertyNames->name);
	if (name && isJSString(name))
	{
		return JSC::asString(name);
	}

	/* For functions (either JSFunction or InternalFunction), fallback to their "native" name property.
	 * Based on JSC::getCalculatedDisplayName, "inlining" the
	 * JSFunction::calculatedDisplayName\InternalFunction::calculatedDisplayName calls */
	if (JSC::JSFunction * function = JSC::jsDynamicCast<JSC::JSFunction *>(m_vm, calleeAsObject))
	{
		// Based on JSC::JSFunction::calculatedDisplayName, skipping the "displayName" property check
		WTF::String actualName = function->name(m_vm);
		if (!actualName.isEmpty() || function->isHostOrBuiltinFunction())
		{
			return JSC::jsString(&m_vm, actualName);
		}

		return JSC::jsString(&m_vm, function->jsExecutable()->inferredName().string());
	}
	if (JSC::InternalFunction * function = JSC::jsDynamicCast<JSC::InternalFunction *>(m_vm, calleeAsObject))
	{
		// Based on JSC::InternalFunction::calculatedDisplayName, skipping the "displayName" property check
		return JSC::jsString(&m_vm, function->name());
	}

	return m_vm.smallStrings.emptyString();
}

// General flow here is based on JSC's appendSourceToError (ErrorInstance.cpp)
bool JSCStackFrame::calculateSourcePositions()
{
	if (!m_codeBlock)
	{
		return false;
	}

	unsigned int bytecodeOffset = hasBytecodeOffset() ? m_bytecodeOffset : 0;

	/* Get the "raw" position info.
	 * Note that we're using m_codeBlock->unlinkedCodeBlock()->expressionRangeForBytecodeOffset rather than m_codeBlock->expressionRangeForBytecodeOffset
	 * in order get the "raw" offsets and avoid the CodeBlock's expressionRangeForBytecodeOffset modifications to the line and column numbers,
	 * (we don't need the column number from it, and we'll calculate the line "fixes" ourselves). */
	int startOffset = 0;
	int endOffset = 0;
	int divotPoint = 0;
	unsigned line = 0;
	unsigned unusedColumn = 0;
	m_codeBlock->unlinkedCodeBlock()->expressionRangeForBytecodeOffset(bytecodeOffset, divotPoint, startOffset, endOffset, line, unusedColumn);
	divotPoint += m_codeBlock->sourceOffset();

	/* On the first line of the source code, it seems that we need to "fix" the column with the starting
	 * offset. We currently use codeBlock->source()->startPosition().m_column.oneBasedInt() as the
	 * offset in the first line rather than codeBlock->firstLineColumnOffset(), which seems simpler
	 * (and what CodeBlock::expressionRangeForBytecodeOffset does). This is because firstLineColumnOffset 
	 * values seems different from what we expect (according to v8's tests) and I haven't dove into the 
	 * relevant parts in JSC (yet) to figure out why. */
	unsigned columnOffset = line ? 0 : m_codeBlock->source()->startPosition().m_column.zeroBasedInt();

	// "Fix" the line number
	JSC::ScriptExecutable * executable = m_codeBlock->ownerScriptExecutable();
	if (executable->hasOverrideLineNumber())
	{
		line = executable->overrideLineNumber();
	}
	else
	{
		line += executable->firstLine();
	}

	// Calculate the staring\ending offsets of the entire expression
	int expressionStart = divotPoint - startOffset;
	int expressionStop = divotPoint + endOffset;

	// Make sure the range is valid
	StringView sourceString = m_codeBlock->source()->source();
	if (!expressionStop || expressionStart > static_cast<int>(sourceString.length()))
	{
		return false;
	}

	// Search for the beginning of the line
	unsigned int lineStart = expressionStart;
	while ((lineStart > 0) && ('\n' != sourceString[lineStart - 1]))
	{
		lineStart--;
	}
	// Search for the end of the line
	unsigned int lineStop = expressionStop;
	unsigned int sourceLength = sourceString.length();
	while ((lineStop < sourceLength) && ('\n' != sourceString[lineStop]))
	{
		lineStop++;
	}

	/* Finally, store the source "positions" info.
	 * Notes:
	 * - The retrieved column seem to point the "end column". To make sure we're current, we'll calculate the
	 *   columns ourselves, since we've already found where the line starts. Note that in v8 it should be 0-based
	 *   here (in contrast the 1-based column number in v8::StackFrame).
	 * - The static_casts are ugly, but comes from differences between JSC and v8's api, and should be OK
	 *   since no source should be longer than "max int" chars. 
	 * TODO: If expressionStart == expressionStop, then m_endColumn will be equal to m_startColumn. Should we handle
	 *		 this case?
	 */
	m_sourcePositions.expressionStart = WTF::OrdinalNumber::fromZeroBasedInt(expressionStart);
	m_sourcePositions.expressionStop  = WTF::OrdinalNumber::fromZeroBasedInt(expressionStop);
	m_sourcePositions.line			  = WTF::OrdinalNumber::fromOneBasedInt(static_cast<int>(line));
	m_sourcePositions.startColumn	  = WTF::OrdinalNumber::fromZeroBasedInt((expressionStart - lineStart) + columnOffset);
	m_sourcePositions.endColumn		  = WTF::OrdinalNumber::fromZeroBasedInt(m_sourcePositions.startColumn.zeroBasedInt() + (expressionStop - expressionStart));
	m_sourcePositions.lineStart		  = WTF::OrdinalNumber::fromZeroBasedInt(static_cast<int>(lineStart));
	m_sourcePositions.lineStop		  = WTF::OrdinalNumber::fromZeroBasedInt(static_cast<int>(lineStop));

	return true;
}

}} // v8::jscshim