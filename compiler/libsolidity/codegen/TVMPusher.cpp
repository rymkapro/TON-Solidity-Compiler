/*
 * Copyright 2018-2019 TON DEV SOLUTIONS LTD.
 *
 * Licensed under the  terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License.
 *
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the  GNU General Public License for more details at: https://www.gnu.org/licenses/gpl-3.0.html
 */
/**
 * @author TON Labs <connect@tonlabs.io>
 * @date 2019
 */

#include "TVMCommons.hpp"
#include "TVMPusher.hpp"
#include "TVMContractCompiler.hpp"
#include "TVMExpressionCompiler.hpp"

using namespace solidity::frontend;

StackPusherHelper::StackPusherHelper(const TVMCompilerContext *ctx, const int stackSize) :
		m_ctx(ctx),
		m_structCompiler{new StructCompiler{this,
		                                    ctx->notConstantStateVariables(),
		                                    256 + (m_ctx->storeTimestampInC4()? 64 : 0) + 1, // pubkey + timestamp + constructor_flag
		                                    1,
		                                    true}} {
	m_stack.change(stackSize);
}

void StackPusherHelper::pushLog(const std::string& str) {
	if (!TVMContractCompiler::g_without_logstr) {
		push(0, "PRINTSTR " + str);
	}
}

StructCompiler &StackPusherHelper::structCompiler() {
	return *m_structCompiler;
}

void StackPusherHelper::generateC7ToT4Macro() {
	pushLines(R"(
.macro	c7_to_c4
GETGLOB 2
NEWC
STU 256
)");
	if (ctx().storeTimestampInC4()) {
		pushLines(R"(
GETGLOB 3
STUR 64
)");
	}
	pushLines(R"(
GETGLOB 6
STUR 1
)");
	if (!ctx().notConstantStateVariables().empty()) {
		structCompiler().stateVarsToBuilder();
	}
	pushLines(R"(
ENDC
POP C4
)");
	push(0, " ");
}

bool
StackPusherHelper::prepareValueForDictOperations(Type const *keyType, Type const *dictValueType, bool isValueBuilder) {
	// value
	if (isIntegralType(dictValueType)) {
		if (!isValueBuilder) {
			push(0, "NEWC");
			push(0, storeIntegralOrAddress(dictValueType, false));
			return true;
		}
	} else if (dictValueType->category() == Type::Category::Struct) {
		if (StructCompiler::isCompatibleWithSDK(lengthOfDictKey(keyType), to<StructType>(dictValueType))) {
			if (isValueBuilder) {
				return true;
			} else {
				StructCompiler sc{this, to<StructType>(dictValueType)};
				sc.tupleToBuilder();
				return true;
			}
		} else {
			if (!isValueBuilder) {
				StructCompiler sc{this, to<StructType>(dictValueType)};
				sc.tupleToBuilder();
			}
			push(+1, "NEWC");
			push(-2 + 1, "STBREF");
			return true;
		}
	} else if (isUsualArray(dictValueType)) {
		if (!isValueBuilder) {
			push(-1 + 2, "UNPAIR"); // size dict
			push(0, "SWAP"); // dict size
			push(+1, "NEWC"); // dict size builder
			push(-1, "STU 32"); // dict builder
			push(-1, "STDICT"); // builder
			return true;
		}
	} else if (to<TvmCellType>(dictValueType) || (to<ArrayType>(dictValueType) && to<ArrayType>(dictValueType)->isByteArray())) {
		if (isValueBuilder) {
			push(0, "ENDC");
			return false;
		}
	} else if (dictValueType->category() == Type::Category::Mapping) {
		if (!isValueBuilder) {
			push(+1, "NEWC"); // dict builder
			push(-1, "STDICT"); // builder
			return true;
		}
	} else if (dictValueType->category() == Type::Category::VarInteger) {
		if (!isValueBuilder) {
			push(+1, "NEWC"); // value builder
			push(0, "SWAP"); // builder value
			push(-1, "STVARUINT32"); // builder
			return true;
		}
	}

	return isValueBuilder;
}

void StackPusherHelper::setDict(Type const &keyType, Type const &valueType, bool isValueBuilder, ASTNode const &node) {
	// stack: value index dict
	int keyLength = lengthOfDictKey(&keyType);
	pushInt(keyLength);

	// stack: value index dict keyBitLength
	string dict_cmd;
	switch (valueType.category()) {
		case Type::Category::Address:
		case Type::Category::Contract:
			if (isValueBuilder) {
				dict_cmd = "DICT" + typeToDictChar(&keyType) + "SETB";
			} else {
				dict_cmd = "DICT" + typeToDictChar(&keyType) + "SET";
			}
			break;
		case Type::Category::TvmCell:
			solAssert(!isValueBuilder, "");
			dict_cmd = "DICT" + typeToDictChar(&keyType) + "SETREF";
			break;
		case Type::Category::Struct:
			if (StructCompiler::isCompatibleWithSDK(keyLength, to<StructType>(&valueType))) {
				if (isValueBuilder) {
					dict_cmd = "DICT" + typeToDictChar(&keyType) + "SETB";
				} else {
					dict_cmd = "DICT" + typeToDictChar(&keyType) + "SET";
				}
			} else {
				solAssert(isValueBuilder, "");
				dict_cmd = "DICT" + typeToDictChar(&keyType) + "SETB";
			}
			break;
		case Type::Category::Integer:
		case Type::Category::Bool:
		case Type::Category::FixedBytes:
		case Type::Category::Enum:
		case Type::Category::VarInteger:
			solAssert(isValueBuilder, "");
			dict_cmd = "DICT" + typeToDictChar(&keyType) + "SETB";
			break;
		case Type::Category::Array:
			if (!to<ArrayType>(&valueType)->isByteArray()) {
				solAssert(isValueBuilder, "");
				dict_cmd = "DICT" + typeToDictChar(&keyType) + "SETB";
			} else {
				solAssert(!isValueBuilder, "");
				dict_cmd = "DICT" + typeToDictChar(&keyType) + "SETREF";
			}
			break;
		case Type::Category::Mapping:
			solAssert(isValueBuilder, "");
			dict_cmd = "DICT" + typeToDictChar(&keyType) + "SETB";
			break;
		default:
			cast_error(node, "Unsupported value type: " + valueType.toString());
	}

	push(-3, dict_cmd);
}

void StackPusherHelper::tryPollLastRetOpcode() {
	if (m_code.lines.empty()) {
		return;
	}
	if (std::regex_match(m_code.lines.back(), std::regex("(\t*)RET"))) {
		m_code.lines.pop_back();
	}
}

void StackPusherHelper::append(const CodeLines &oth) {
	m_code.append(oth);
}

void StackPusherHelper::addTabs(const int qty) {
	m_code.addTabs(qty);
}

void StackPusherHelper::subTabs(const int qty) {
	m_code.subTabs(qty);
}

void StackPusherHelper::pushCont(const CodeLines &cont, const string &comment) {
	if (comment.empty())
		push(0, "PUSHCONT {");
	else
		push(0, "PUSHCONT { ; " + comment);
	for (const auto& l : cont.lines)
		push(0, string("\t") + l);
	push(+1, "}"); // adjust stack
}

void StackPusherHelper::generateGlobl(const string &fname, const bool isPublic) {
	push(0, ".globl\t" + fname);
	if (isPublic) {
		push(0, ".public\t" + fname);
	}
	push(0, ".type\t"  + fname + ", @function");
}

void StackPusherHelper::generateInternal(const string &fname, const int id) {
	push(0, ".internal-alias :" + fname + ",        " + toString(id));
	push(0, ".internal\t:" + fname);
}

void StackPusherHelper::generateMacro(const string &functionName) {
	push(0, ".macro " + functionName);
}

CodeLines StackPusherHelper::code() const {
	return m_code;
}

const TVMCompilerContext &StackPusherHelper::ctx() const {
	return *m_ctx;
}

void StackPusherHelper::push(int stackDiff, const string &cmd) {
	m_code.push(cmd);
	m_stack.change(stackDiff);
}

void StackPusherHelper::startContinuation() {
	m_code.startContinuation();
}

void StackPusherHelper::endContinuation() {
	m_code.endContinuation();
}

TVMStack &StackPusherHelper::getStack() {
	return m_stack;
}

void StackPusherHelper::pushLines(const std::string &lines) {
	std::istringstream stream{lines};
	std::string line;
	while (std::getline(stream, line)) {
		push(0, line);
	}
}

void StackPusherHelper::untuple(int n) {
	solAssert(0 <= n, "");
	if (n <= 15) {
		push(-1 + n, "UNTUPLE " + toString(n));
	} else {
		solAssert(n <= 255, "");
		pushInt(n);
		push(-2 + n, "UNTUPLEVAR");
	}
}

void StackPusherHelper::index(int index) {
	solAssert(0 <= index, "");
	if (index <= 15) {
		push(-1 + 1, "INDEX " + toString(index));
	} else {
		solAssert(index <= 254, "");
		pushInt(index);
		push(-2 + 1, "INDEXVAR");
	}
}

void StackPusherHelper::set_index(int index) {
	solAssert(0 <= index, "");
	if (index <= 15) {
		push(-2 + 1, "SETINDEX " + toString(index));
	} else {
		solAssert(index <= 254, "");
		pushInt(index);
		push(-1 - 2 + 1, "SETINDEXVAR");
	}
}

void StackPusherHelper::tuple(int qty) {
	solAssert(0 <= qty, "");
	if (qty <= 15) {
		push(-qty + 1, "TUPLE " + toString(qty));
	} else {
		solAssert(qty <= 255, "");
		pushInt(qty);
		push(-1 - qty + 1, "TUPLEVAR");
	}
}

void StackPusherHelper::resetAllStateVars() {
	push(0, ";; set default state vars");
	for (VariableDeclaration const *variable: ctx().notConstantStateVariables()) {
		pushDefaultValue(variable->type());
		setGlob(variable);
	}
	push(0, ";; end set default state vars");
}

void StackPusherHelper::getGlob(VariableDeclaration const *vd) {
	const int index = ctx().getStateVarIndex(vd);
	getGlob(index);
}

void StackPusherHelper::getGlob(int index) {
	solAssert(index >= 0, "");
	if (index <= 31) {
		push(+1, "GETGLOB " + toString(index));
	} else {
		solAssert(index < 255, "");
		pushInt(index);
		push(-1 + 1, "GETGLOBVAR");
	}
}

void StackPusherHelper::setGlob(int index) {
	if (index <= 31) {
		push(-1, "SETGLOB " + toString(index));
	} else {
		solAssert(index < 255, "");
		pushInt(index);
		push(-1 - 1, "SETGLOBVAR");
	}
}

void StackPusherHelper::setGlob(VariableDeclaration const *vd) {
	const int index = ctx().getStateVarIndex(vd);
	solAssert(index >= 0, "");
	setGlob(index);
}

void StackPusherHelper::pushS(int i) {
	solAssert(i >= 0, "");
	if (i == 0) {
		push(+1, "DUP");
	} else {
		push(+1, "PUSH S" + toString(i));
	}
}

void StackPusherHelper::pushInt(int i) {
	push(+1, "PUSHINT " + toString(i));
}

void StackPusherHelper::loadArray(bool directOrder) {
	pushLines(R"(LDU 32
LDDICT
ROTREV
PAIR
)");
	if (directOrder) {
		exchange(0, 1);
	}
	push(-1 + 2, ""); // fix stack
	// stack: array slice
}

void StackPusherHelper::preLoadArray() {
	pushLines(R"(LDU 32
PLDDICT
PAIR
)");
	push(-1 + 1, ""); // fix stack
	// stack: array
}

void StackPusherHelper::load(const Type *type) {
	if (isUsualArray(type)) {
		loadArray();
	} else {
		TypeInfo ti{type};
		solAssert(ti.isNumeric, "");
		string cmd = ti.isSigned ? "LDI " : "LDU ";
		push(-1 + 2, cmd + toString(ti.numBits));
	}
}

void StackPusherHelper::preload(const Type *type) {
	if (isUsualArray(type)) {
		preLoadArray();
	} else if (type->category() == Type::Category::Mapping) {
		push(0, "PLDDICT");
	} else if (type->category() == Type::Category::VarInteger) {
		push(0, "LDVARUINT32");
		push(0, "DROP");
	} else {
		TypeInfo ti{type};
		solAssert(ti.isNumeric, "");
		string cmd = ti.isSigned ? "PLDI " : "PLDU ";
		push(-1 + 1, cmd + toString(ti.numBits));
	}
}

void StackPusherHelper::pushZeroAddress() {
	push(+1, "PUSHSLICE x8000000000000000000000000000000000000000000000000000000000000000001_");
}

void StackPusherHelper::addBinaryNumberToString(std::string &s, u256 value, int bitlen) {
	for (int i = 0; i < bitlen; ++i) {
		s += value % 2 == 0? "0" : "1";
		value /= 2;
	}
	std::reverse(s.rbegin(), s.rbegin() + bitlen);
}

std::string StackPusherHelper::binaryStringToSlice(const std::string &_s) {
	std::string s = _s;
	bool haveCompletionTag = false;
	if (s.size() % 4 != 0) {
		haveCompletionTag = true;
		s += "1";
		s += std::string((4 - s.size() % 4) % 4, '0');
	}
	std::string ans;
	for (int i = 0; i < static_cast<int>(s.length()); i += 4) {
		int x = stoi(s.substr(i, 4), nullptr, 2);
		std::stringstream sstream;
		sstream << std::hex << x;
		ans += sstream.str();
	}
	if (haveCompletionTag) {
		ans += "_";
	}
	return ans;
}

std::string StackPusherHelper::gramsToBinaryString(Literal const *literal) {
	Type const* type = literal->annotation().type;
	u256 value = type->literalValue(literal);
	return gramsToBinaryString(value);
}

std::string StackPusherHelper::gramsToBinaryString(u256 value) {
	std::string s;
	int len = 256;
	for (int i = 0; i < 256; ++i) {
		if (value == 0) {
			len = i;
			break;
		}
		s += value % 2 == 0? "0" : "1";
		value /= 2;
	}
	solAssert(len < 120, "Gram value should fit 120 bit");
	while (len % 8 != 0) {
		s += "0";
		len++;
	}
	std::reverse(s.rbegin(), s.rbegin() + len);
	len = len/8;
	std::string res;
	for (int i = 0; i < 4; ++i) {
		res += len % 2 == 0? "0" : "1";
		len /= 2;
	}
	std::reverse(res.rbegin(), res.rbegin() + 4);
	return res + s;
}

std::string StackPusherHelper::literalToSliceAddress(Literal const *literal, bool pushSlice) {
	Type const* type = literal->annotation().type;
	u256 value = type->literalValue(literal);
//		addr_std$10 anycast:(Maybe Anycast) workchain_id:int8 address:bits256 = MsgAddressInt;
	std::string s;
	s += "10";
	s += "0";
	s += std::string(8, '0');
	addBinaryNumberToString(s, value);
	if (pushSlice)
		push(+1, "PUSHSLICE x" + binaryStringToSlice(s));
	return s;
}

bool StackPusherHelper::tryImplicitConvert(Type const *leftType, Type const *rightType) {
	if (leftType->category() == Type::Category::FixedBytes && rightType->category() == Type::Category::StringLiteral) {
		auto stringLiteralType = to<StringLiteralType>(rightType);
		u256 value = 0;
		for (char c : stringLiteralType->value()) {
			value = value * 256 + c;
		}
		push(+1, "PUSHINT " + toString(value));
		return true;
	}
	return false;
}

void StackPusherHelper::push(const CodeLines &codeLines) {
	for (const std::string& s : codeLines.lines) {
		push(0, s);
	}
}

void StackPusherHelper::pushPrivateFunctionOrMacroCall(const int stackDelta, const string &fname) {
	push(stackDelta, "CALL $" + fname + "$");
}

void StackPusherHelper::pushCall(const string &functionName, const FunctionType *ft) {
	int params  = ft->parameterTypes().size();
	int retVals = ft->returnParameterTypes().size();
	push(-params + retVals, "CALL $" + functionName + "$");
}

void StackPusherHelper::drop(int cnt) {
	solAssert(cnt >= 0, "");
	if (cnt == 0)
		return;

	if (cnt == 1) {
		push(-1, "DROP");
	} else if (cnt == 2) {
		push(-2, "DROP2");
	} else {
		if (cnt > 15) {
			pushInt(cnt);
			push(-(cnt + 1), "DROPX");
		} else {
			push(-cnt, "BLKDROP " + toString(cnt));
		}
	}
}

void StackPusherHelper::blockSwap(int m, int n) {
	if (m == 0 || n == 0) {
		return;
	}
	if (m == 1 && n == 1) {
		exchange(0, 1);
	} else if (m == 2 && n == 2) {
		push(0, "SWAP2");
	} else {
		push(0, "BLKSWAP " + toString(m) + ", " + toString(n));
	}
}

void StackPusherHelper::reverse(int i, int j) {
	solAssert(i >= 2, "");
	solAssert(j >= 0, "");
	if (i == 2 && j == 0) {
		push(0, "SWAP");
	} else if (i == 3 && j == 0) {
		push(0, "XCHG s2");
	} else if (i - 2 <= 15 && j <= 15) {
		push(0, "REVERSE " + toString(i) + ", " + toString(j));
	} else {
		pushInt(i);
		pushInt(j);
		push(-2, "REVX");
	}
}

void StackPusherHelper::dropUnder(int leftCount, int droppedCount) {
	// drop dropCount elements that are situated under top leftCount elements
	solAssert(leftCount >= 0, "");
	solAssert(droppedCount >= 0, "");

	auto f = [this, leftCount, droppedCount](){
		if (droppedCount > 15 || leftCount > 15) {
			pushInt(droppedCount);
			pushInt(leftCount);
			push(-2, "BLKSWX");
		} else {
			blockSwap(droppedCount, leftCount);
		}
		drop(droppedCount);
	};

	if (droppedCount == 0) {
		// nothing do
	} else if (leftCount == 0) {
		drop(droppedCount);
	} else if (droppedCount == 1) {
		if (leftCount == 1) {
			push(-1, "NIP");
		} else {
			f();
		}
	} else if (droppedCount == 2) {
		if (leftCount == 1) {
			push(-1, "NIP");
			push(-1, "NIP");
		} else {
			f();
		}
	} else {
		if (leftCount == 1) {
			exchange(0, droppedCount);
			drop(droppedCount);
		} else {
			f();
		}
	}
}

void StackPusherHelper::exchange(int i, int j) {
	solAssert(i <= j, "");
	solAssert(i >= 0, "");
	solAssert(j >= 1, "");
	if (i == 0 && j <= 255) {
		if (j == 1) {
			push(0, "SWAP");
		} else if (j <= 15) {
			push(0, "XCHG s" + toString(j));
		} else {
			push(0, "XCHG s0,s" + toString(j));
		}
	} else if (i == 1 && 2 <= j && j <= 15) {
		push(0, "XCHG s1,s" + toString(j));
	} else if (1 <= i && i < j && j <= 15) {
		push(0, "XCHG s" + toString(i) + ",s" + toString(j));
	} else if (j <= 255) {
		exchange(0, i);
		exchange(0, j);
		exchange(0, i);
	} else {
		solAssert(false, "");
	}
}

void StackPusherHelper::restoreKeyAfterDictOperations(Type const *keyType, ASTNode const &node) {
	if (isStringOrStringLiteralOrBytes(keyType)) {
		cast_error(node, "Unsupported for mapping key type: " + keyType->toString(true));
	}
}

TypePointer StackPusherHelper::parseIndexType(Type const *type) {
	if (to<ArrayType>(type)) {
		return TypePointer(new IntegerType(32));
	}
	if (auto mappingType = to<MappingType>(type)) {
		return mappingType->keyType();
	}
	if (auto currencyType = to<ExtraCurrencyCollectionType>(type)) {
		return currencyType->keyType();
	}
	solAssert(false, "");
}

TypePointer StackPusherHelper::parseValueType(IndexAccess const &indexAccess) {
	if (auto currencyType = to<ExtraCurrencyCollectionType>(indexAccess.baseExpression().annotation().type)) {
		return currencyType->realValueType();
	}
	return indexAccess.annotation().type;
}

bool StackPusherHelper::tryAssignParam(Declaration const *name) {
	auto& stack = getStack();
	if (stack.isParam(name)) {
		int idx = stack.getOffset(name);
		solAssert(idx >= 0, "");
		if (idx == 0) {
			// nothing
		} else if (idx == 1) {
			push(-1, "NIP");
		} else {
			push(-1, "POP s" + toString(idx));
		}
		return true;
	}
	return false;
}

void StackPusherHelper::ensureValueFitsType(const ElementaryTypeNameToken &typeName, const ASTNode &node) {
	push(0, ";; " + typeName.toString());
	switch (typeName.token()) {
		case Token::IntM:
			push(0, "FITS " + toString(typeName.firstNumber()));
			break;
		case Token::UIntM:
			push(0, "UFITS " + toString(typeName.firstNumber()));
			break;
		case Token::BytesM:
			push(0, "UFITS " + toString(8 * typeName.firstNumber()));
			break;
		case Token::Int:
			push(0, "FITS 256");
			break;
		case Token::Address:
			// Address is a slice
			break;
		case Token::UInt:
			push(0, "UFITS 256");
			break;
		case Token::Bool:
			push(0, "FITS 1");
			break;
		default:
			cast_error(node, "Unimplemented casting");
	}
}

void StackPusherHelper::encodeParameter(Type const *type, StackPusherHelper::EncodePosition &position,
                                        const std::function<void()> &pushParam, ASTNode const *node) {
	// stack: builder...
	if (auto structType = to<StructType>(type)) {
		pushParam(); // builder... struct
		encodeStruct(structType, node, position); // stack: builder...
	} else {
		if (position.needNewCell(type)) {
			push(+1, "NEWC");
		}

		if (isIntegralType(type) || isAddressOrContractType(type)) {
			pushParam();
			push(-1, storeIntegralOrAddress(type, true));
		} else if (auto arrayType = to<ArrayType>(type)) {
			if (arrayType->isByteArray()) {
				pushParam();
				push(-1, "STREFR");
			} else {
				pushParam();
				// builder array
				push(-1 + 2, "UNPAIR"); // builder size dict
				exchange(0, 2); // dict size builder
				push(-1, "STU 32"); // dict builder
				push(-1, "STDICT"); // builder
			}
		} else if (to<TvmCellType>(type)) {
			pushParam();
			push(-1, "STREFR");
		} else if (to<MappingType>(type)) {
			pushParam();
			push(0, "SWAP");
			push(-1, "STDICT");
		} else {
			cast_error(*node, "Unsupported type for encoding: " + type->toString());
		}
	}
}

void
StackPusherHelper::encodeParameters(const std::vector<Type const *> &types, const std::vector<ASTNode const *> &nodes,
                                    const std::function<void(size_t)> &pushParam,
                                    StackPusherHelper::EncodePosition &position) {
	// builder must be situated on top stack
	solAssert(types.size() == nodes.size(), "");
	for (size_t idx = 0; idx < types.size(); idx++) {
		auto type = types[idx];
		encodeParameter(type, position, [&](){pushParam(idx);}, nodes[idx]);
	}
	for (int idx = 0; idx < position.countOfCreatedBuilders(); idx++) {
		push(-1, "STBREFR");
	}
}

int StackPusherHelper::encodeFunctionAndParams(const string &functionName, const std::vector<Type const *> &types,
                                                const std::vector<ASTNode const *> &nodes,
                                                const std::function<void(size_t)> &pushParam,
                                                const StackPusherHelper::ReasonOfOutboundMessage &reason) {
	push(+1, "NEWC");
	push(+1, "PUSHINT $" + functionName + "$");
	switch (reason) {
		case ReasonOfOutboundMessage::FunctionReturnExternal:
			push(+1, "PUSHINT " + to_string(0x80000000));
			push(-1, "OR");
			break;

		case ReasonOfOutboundMessage::EmitEventExternal:
			push(+1, "PUSHINT " + to_string(0x7fffffff));
			push(-1, "AND");
			break;

		default:
			break;
	}

	push(-1, "STUR 32");
	EncodePosition position{32};

	encodeParameters(types, nodes, pushParam, position);

	if (position.countOfCreatedBuilders() == 0)
		return TvmConst::CellBitLength - position.restBits();
	return -1;
}

void StackPusherHelper::prepareKeyForDictOperations(Type const *key) {
	// stack: key dict
	if (isStringOrStringLiteralOrBytes(key)) {
		push(+1, "PUSH s1"); // str dict str
		push(-1 + 1, "HASHCU"); // str dict hash
		push(-1, "POP s2"); // hash dict
	}
}

std::pair<std::string, int>
StackPusherHelper::int_msg_info(const std::set<int> &isParamOnStack, const std::map<int, std::string> &constParams) {
	// int_msg_info$0  ihr_disabled:Bool  bounce:Bool(#1)  bounced:Bool
	//                 src:MsgAddress  dest:MsgAddressInt(#4)
	//                 value:CurrencyCollection(#5,#6)  ihr_fee:Grams  fwd_fee:Grams
	//                 created_lt:uint64  created_at:uint32
	//                 = CommonMsgInfoRelaxed;

	// currencies$_ grams:Grams other:ExtraCurrencyCollection = CurrencyCollection;

	const std::vector<int> zeroes {1, 1, 1,
									2, 2,
						            4, 1, 4, 4,
						            64, 32};
	std::string bitString = "0";
	int maxBitStringSize = 0;
	push(+1, "NEWC");
	for (int param = 0; param < static_cast<int>(zeroes.size()); ++param) {
		solAssert(constParams.count(param) == 0 || isParamOnStack.count(param) == 0, "");

		if (constParams.count(param) != 0) {
			bitString += constParams.at(param);
		} else if (isParamOnStack.count(param) == 0) {
			bitString += std::string(zeroes[param], '0');
			solAssert(param != TvmConst::int_msg_info::dest, "");
		} else {
			appendToBuilder(bitString);
			bitString = "";
			switch (param) {
				case TvmConst::int_msg_info::bounce:
					push(-1, "STI 1");
					++maxBitStringSize;
					break;
				case TvmConst::int_msg_info::dest:
					push(-1, "STSLICE");
					maxBitStringSize += AddressInfo::maxBitLength();
					break;
				case TvmConst::int_msg_info::grams:
					exchange(0, 1);
					push(-1, "STGRAMS");
					maxBitStringSize += 4 + 16 * 8;
					// var_uint$_ {n:#} len:(#< n) value:(uint (len * 8)) = VarUInteger n;
					// nanograms$_ amount:(VarUInteger 16) = Grams;
					break;
				case TvmConst::int_msg_info::currency:
					push(-1, "STDICT");
					break;
				default:
					solAssert(false, "");
			}
		}
	}
	maxBitStringSize += bitString.size();
	return {bitString, maxBitStringSize};
}

void StackPusherHelper::appendToBuilder(const std::string &bitString) {
	// stack: builder
	if (bitString.empty()) {
		return;
	}

	size_t count = std::count_if(bitString.begin(), bitString.end(), [](char c) { return c == '0'; });
	if (count == bitString.size()) {
		stzeroes(count);
	} else {
		const std::string hex = binaryStringToSlice(bitString);
		if (hex.length() * 4 <= 8 * 7 + 1) {
			push(0, "STSLICECONST x" + hex);
		} else {
			push(+1, "PUSHSLICE x" + binaryStringToSlice(bitString));
			push(-1, "STSLICER");
		}
	}
}

void StackPusherHelper::stzeroes(int qty) {
	if (qty > 0) {
		// builder
		if (qty == 1) {
			push(0, "STSLICECONST 0");
		} else {
			pushInt(qty); // builder qty
			push(-1, "STZEROES");
		}
	}
}

void StackPusherHelper::stones(int qty) {
	if (qty > 0) {
		// builder
		if (qty == 1) {
			push(0, "STSLICECONST 1");
		} else {
			pushInt(qty); // builder qty
			push(-1, "STONES");
		}
	}
}

void StackPusherHelper::sendrawmsg() {
	push(-2, "SENDRAWMSG");
}

void StackPusherHelper::sendIntMsg(const std::map<int, Expression const *> &exprs,
                                   const std::map<int, std::string> &constParams, const std::function<int()> &pushBody,
                                   const std::function<void()> &pushSendrawmsgFlag) {
	std::set<int> isParamOnStack;
	for (auto &[param, expr] : exprs | boost::adaptors::reversed) {
		isParamOnStack.insert(param);
		TVMExpressionCompiler{*this}.compileNewExpr(expr);
	}
	auto [biString, builderSize] = int_msg_info(isParamOnStack, constParams);
	// stack: builder

	if (pushBody) {
		appendToBuilder(biString + "0"); // there is no StateInit
		++builderSize;

		int bodySize = pushBody();
		// stack: builder body
		exchange(0, 1); // stack: body builder
		if (bodySize == -1) {
			// check that brembits(builder) > sbits(body)
			pushLines(R"(
; merge body and builder
PUSH S1
BBITS
PUSH S1
BREMBITS
GREATER
PUSHCONT {
	STSLICECONST 1
	STBREF
}
PUSHCONT {
	STSLICECONST 0
	STB
}
IFELSE
)");
			push(-2 + 1, "");
		} else if (bodySize + builderSize <= TvmConst::CellBitLength) {
			stzeroes(1);
			push(-1, "STB");
		} else {
			stones(1);
			push(-1, "STBREF");
		}
	} else {
		appendToBuilder(biString + "00"); // there is no StateInit and no body
	}

	// stack: builder'
	push(0, "ENDC"); // stack: cell
	if (pushSendrawmsgFlag) {
		pushSendrawmsgFlag();
	} else {
		pushInt(TvmConst::SENDRAWMSG::DefaultFlag);
	}
	sendrawmsg();
}

CodeLines solidity::frontend::switchSelectorIfNeed(FunctionDefinition const *f) {
	TVMScanner scanner{*f};
	CodeLines code;
	if (scanner.havePrivateFunctionCall) {
		code.push("PUSHINT 1");
		code.push("CALL 1");
	}
	return code;
}

TVMStack::TVMStack() : m_size(0) {}

int TVMStack::size() const {
	return m_size;
}

void TVMStack::change(int diff) {
	m_size += diff;
	solAssert(m_size >= 0, "");
}

bool TVMStack::isParam(Declaration const *name) const {
	return m_params.count(name) > 0;
}

void TVMStack::add(Declaration const *name, bool doAllocation) {
	solAssert(m_params.count(name) == 0, "");
	m_params[name] = doAllocation? m_size++ : m_size - 1;
}

int TVMStack::getOffset(Declaration const *name) const {
	solAssert(isParam(name), "");
	return getOffset(m_params.at(name));
}

int TVMStack::getOffset(int stackPos) const {
	return m_size - 1 - stackPos;
}

int TVMStack::getStackSize(Declaration const *name) const {
	return m_params.at(name);
}

void TVMStack::ensureSize(int savedStackSize, const string &location) const {
	solAssert(savedStackSize == m_size, "stack: " + toString(savedStackSize)
	                                    + " vs " + toString(m_size) + " at " + location);
}

string CodeLines::str(const string &indent) const {
	std::ostringstream o;
	for (const string& s : lines) {
		o << indent << s << endl;
	}
	return o.str();
}

void CodeLines::addTabs(const int qty) {
	tabQty += qty;
}

void CodeLines::subTabs(const int qty) {
	tabQty -= qty;
}

void CodeLines::startContinuation() {
	push("PUSHCONT {");
	++tabQty;
}

void CodeLines::endContinuation() {
	--tabQty;
	push("}");
	solAssert(tabQty >= 0, "");
}

void CodeLines::push(const string &cmd) {
	if (cmd.empty() || cmd == "\n") {
		return;
	}

	// space means empty line
	if (cmd == " ")
		lines.emplace_back("");
	else {
		solAssert(tabQty >= 0, "");
		lines.push_back(std::string(tabQty, '\t') + cmd);
	}
}

void CodeLines::append(const CodeLines &oth) {
	for (const auto& s : oth.lines) {
		lines.push_back(std::string(tabQty, '\t') + s);
	}
}

void TVMCompilerContext::addEvent(EventDefinition const *event) {
	std::string name = event->name();
	solAssert(m_events.count(name) == 0, "Duplicate event " + name);
	m_events[name] = event;
}

void TVMCompilerContext::addFunction(FunctionDefinition const *_function) {
	if (!_function->isConstructor()) {
		string name = functionName(_function);
		m_functions[name] = _function;
	}
}

void TVMCompilerContext::initMembers(ContractDefinition const *contract) {
	solAssert(!m_contract, "");
	m_contract = contract;
	for (ContractDefinition const* c : getContractsChain(contract)) {
		for (EventDefinition const *event : c->events())
			addEvent(event);
	}
	for (const auto &pair : getContractFunctionPairs(contract)) {
		m_function2contract.insert(pair);
	}

	for (FunctionDefinition const* f : contract->definedFunctions()) {
		ignoreIntOverflow |= f->name() == "tvm_ignore_integer_overflow";
		m_haveSetDestAddr |=  f->name() == "tvm_set_ext_dest_address";
		haveFallback |= f->isFallback();
		haveOnBounce |= f->name() == "onBounce";
		haveReceive  |= f->isReceive();
	}
	ignoreIntOverflow |= m_pragmaHelper.haveIgnoreIntOverflow();
	for (const auto f : getContractFunctions(contract)) {
		if (isPureFunction(f))
			continue;
		addFunction(f);
	}
	for (const auto &pair : getContractFunctionPairs(contract)) {
		auto f = pair.first;
		if (!isTvmIntrinsic(f->name()) && !isPureFunction(f) && !isFunctionForInlining(f)) {
			m_functionsList.push_back(f);
		}
	}

	for (VariableDeclaration const *variable: notConstantStateVariables()) {
		m_stateVarIndex[variable] = 10 + m_stateVarIndex.size();
	}
}

TVMCompilerContext::TVMCompilerContext(ContractDefinition const *contract,
                                       PragmaDirectiveHelper const &pragmaHelper) : m_pragmaHelper{pragmaHelper} {
	initMembers(contract);
}

int TVMCompilerContext::getStateVarIndex(VariableDeclaration const *variable) const {
	return m_stateVarIndex.at(variable);
}

std::vector<VariableDeclaration const *> TVMCompilerContext::notConstantStateVariables() const {
	std::vector<VariableDeclaration const*> variableDeclarations;
	std::vector<ContractDefinition const*> mainChain = getContractsChain(getContract());
	for (ContractDefinition const* contract : mainChain) {
		for (VariableDeclaration const *variable: contract->stateVariables()) {
			if (!variable->isConstant()) {
				variableDeclarations.push_back(variable);
			}
		}
	}
	return variableDeclarations;
}

PragmaDirectiveHelper const &TVMCompilerContext::pragmaHelper() const {
	return m_pragmaHelper;
}

bool TVMCompilerContext::haveTimeInAbiHeader() const {
	if (m_pragmaHelper.abiVersion() == 1) {
		return true;
	}
	if (m_pragmaHelper.abiVersion() == 2) {
		return m_pragmaHelper.haveTime() || afterSignatureCheck() == nullptr;
	}
	solAssert(false, "");
}

bool TVMCompilerContext::isStdlib() const {
	return m_contract->name() == "stdlib";
}

bool TVMCompilerContext::haveSetDestAddr() const {
	return m_haveSetDestAddr;
}

string TVMCompilerContext::getFunctionInternalName(FunctionDefinition const *_function) const {
	if (isStdlib()) {
		return _function->name();
	}
	if (_function->name() == "onCodeUpgrade") {
		return ":onCodeUpgrade";
	}
	return _function->name() + "_internal";
}

string TVMCompilerContext::getFunctionExternalName(FunctionDefinition const *_function) {
	const string& fname = _function->name();
	solAssert(_function->isPublic(), "Internal error: expected public function: " + fname);
	if (_function->isConstructor()) {
		return "constructor";
	}
	if (_function->isFallback()) {
		return "fallback";
	}
	return fname;
}

bool TVMCompilerContext::isPureFunction(FunctionDefinition const *f) const {
	const auto& vec = getContract(f)->annotation().unimplementedFunctions;
	return std::find(vec.cbegin(), vec.cend(), f) != vec.end();
}

const ContractDefinition *TVMCompilerContext::getContract() const {
	return m_contract;
}

const ContractDefinition *TVMCompilerContext::getContract(const FunctionDefinition *f) const {
	return m_function2contract.at(f);
}

const FunctionDefinition *TVMCompilerContext::getLocalFunction(const string& fname) const {
	return get_from_map(m_functions, fname, nullptr);
}

const EventDefinition *TVMCompilerContext::getEvent(const string &name) const {
	return get_from_map(m_events, name, nullptr);
}

bool TVMCompilerContext::haveFallbackFunction() const {
	return haveFallback;
}

bool TVMCompilerContext::haveReceiveFunction() const {
	return haveReceive;
}

bool TVMCompilerContext::haveOnBounceHandler() const {
	return haveOnBounce;
}

bool TVMCompilerContext::ignoreIntegerOverflow() const {
	return ignoreIntOverflow;
}

std::vector<const EventDefinition *> TVMCompilerContext::events() const {
	std::vector<const EventDefinition*> result;
	for (const auto& [name, event] : m_events) {
		(void)name;
		result.push_back(event);
	}
	return result;
}

FunctionDefinition const *TVMCompilerContext::afterSignatureCheck() const {
	for (FunctionDefinition const* f : m_contract->definedFunctions()) {
		if (f->name() == "afterSignatureCheck") {
			return f;
		}
	}
	return nullptr;
}

bool TVMCompilerContext::storeTimestampInC4() const {
	return haveTimeInAbiHeader() && afterSignatureCheck() == nullptr;
}

StackPusherHelper::EncodePosition::EncodePosition(int bits) :
		restSliceBits{TvmConst::CellBitLength - bits},
		restFef{4},
		qtyOfCreatedBuilders{0}
{

}

bool StackPusherHelper::EncodePosition::needNewCell(Type const *type) {
	ABITypeSize size(type);
	solAssert(0 <= size.maxRefs && size.maxRefs <= 1, "");

	restSliceBits -= size.maxBits;
	restFef -= size.maxRefs;

	if (restSliceBits < 0 || restFef == 0) {
		restSliceBits =  TvmConst::CellBitLength - size.maxBits;
		restFef = 4 - size.maxRefs;
		++qtyOfCreatedBuilders;
		return true;
	}
	return false;
}

int StackPusherHelper::EncodePosition::countOfCreatedBuilders() const {
	return qtyOfCreatedBuilders;
}


void StackPusherHelper::encodeStruct(const StructType* structType, ASTNode const* node, EncodePosition& position) {
	// builder... builder struct
	const int saveStackSize0 = getStack().size() - 2;
	ast_vec<VariableDeclaration> const& members = structType->structDefinition().members();
	const int memberQty = members.size();
	untuple(memberQty); // builder... builder values...
	blockSwap(1, memberQty); // builder... values... builder
	for (int i = 0; i < memberQty; ++i) {
		encodeParameter(members[i]->type(), position, [&]() {
			const int index = getStack().size() - saveStackSize0 - 1 - i;
			pushS(index);
		}, node);
	}

	// builder... values... builder...
	const int builderQty = getStack().size() - saveStackSize0 - memberQty;
	dropUnder(builderQty, memberQty);
}

void StackPusherHelper::pushDefaultValue(Type const* type, bool isResultBuilder) {
	Type::Category cat = type->category();
	switch (cat) {
		case Type::Category::Address:
		case Type::Category::Contract:
			pushZeroAddress();
			if (isResultBuilder) {
				push(+1, "NEWC");
				push(-1, "STSLICE");
			}
			break;
		case Type::Category::Bool:
		case Type::Category::FixedBytes:
		case Type::Category::Integer:
		case Type::Category::Enum:
		case Type::Category::VarInteger:
			push(+1, "PUSHINT 0");
			if (isResultBuilder) {
				push(+1, "NEWC");
				push(-1, storeIntegralOrAddress(type, false));
			}
			break;
		case Type::Category::Array:
			if (to<ArrayType>(type)->isByteArray()) {
				push(+1, "NEWC");
				if (!isResultBuilder) {
					push(0, "ENDC");
				}
				break;
			}
			if (!isResultBuilder) {
				pushInt(0);
				push(+1, "NEWDICT");
				push(-2 + 1, "PAIR");
			} else {
				push(+1, "NEWC");
				pushInt(33);
				push(-1, "STZEROES");
			}
			break;
		case Type::Category::Mapping:
		case Type::Category::ExtraCurrencyCollection:
			solAssert(!isResultBuilder, "");
			push(+1, "NEWDICT");
			break;
		case Type::Category::Struct: {
			auto structType = to<StructType>(type);
			StructCompiler structCompiler{this, structType};
			structCompiler.createDefaultStruct(isResultBuilder);
			break;
		}
		case Type::Category::TvmSlice:
			push(+1, "PUSHSLICE x8_");
			if (isResultBuilder) {
				push(+1, "NEWC");
				push(-1, "STSLICE");
			}
			break;
		case Type::Category::TvmBuilder:
			push(+1, "NEWC");
			break;
		case Type::Category::TvmCell:
			push(+1, "NEWC");
			if (!isResultBuilder) {
				push(0, "ENDC");
			}
			break;
		case Type::Category::Function: {
			solAssert(!isResultBuilder, "");
			auto functionType = to<FunctionType>(type);
			StackPusherHelper pusherHelper(&ctx(), functionType->parameterTypes().size());
			pusherHelper.drop(functionType->parameterTypes().size());
			for (const TypePointer &param : functionType->returnParameterTypes()) {
				pusherHelper.pushDefaultValue(param);
			}
			pushCont(pusherHelper.code());
			break;
		}
		default:
			solAssert(false, "");
	}
}

void StackPusherHelper::getFromDict(Type const& keyType, Type const& valueType, ASTNode const& node,
                                    const DictOperation op,
                                    const bool resultAsSliceForStruct) {
	// stack: index dict
	const Type::Category valueCategory = valueType.category();
	prepareKeyForDictOperations(&keyType);
	int keyLength = lengthOfDictKey(&keyType);
	pushInt(keyLength); // stack: index dict nbits

	StackPusherHelper haveValue(&ctx()); // for Fetch
	haveValue.push(0, "SWAP");

	StackPusherHelper pusherMoveC7(&ctx()); // for MoveToC7

	auto pushContinuationWithDefaultDictValue = [&](){
		StackPusherHelper pusherHelper(&ctx());
		if (valueCategory == Type::Category::Struct) {
			if (resultAsSliceForStruct) {
				pusherHelper.pushDefaultValue(&valueType, true);
				pusherHelper.push(0, "ENDC");
				pusherHelper.push(0, "CTOS");
			} else {
				pusherHelper.pushDefaultValue(&valueType, false);
			}
		} else {
			pusherHelper.pushDefaultValue(&valueType);
		}
		pushCont(pusherHelper.code());
	};

	auto fetchValue = [&](){
		StackPusherHelper noValue(&ctx());
		if (valueCategory == Type::Category::Struct) {
			noValue.push(0, "NULL");
		} else {
			noValue.pushDefaultValue(&valueType, false);
		}

		push(0, "DUP");
		pushCont(haveValue.code());
		pushCont(noValue.code());
		push(-2, "IFELSE");
	};

	auto checkExist = [&](){
		StackPusherHelper nip(&ctx());
		nip.push(+1, ""); // fix stack
		nip.push(-1, "NIP"); // delete value

		push(0, "DUP");
		pushCont(nip.code());
		push(-2, "IF");
	};

	std::string dictOpcode = "DICT" + typeToDictChar(&keyType);
	if (valueCategory == Type::Category::TvmCell) {
		push(-3 + 2, dictOpcode + "GETREF");
		switch (op) {
			case DictOperation::GetFromMapping:
				pushContinuationWithDefaultDictValue();
				push(-2, "IFNOT");
				break;
			case DictOperation::GetFromArray:
				push(-1, "THROWIFNOT " + toString(TvmConst::RuntimeException::ArrayIndexOutOfRange));
				break;
			case DictOperation::Fetch:
				fetchValue();
				break;
			case DictOperation::Exist:
				checkExist();
				break;
		}
	} else if (valueCategory == Type::Category::Struct) {
		if (StructCompiler::isCompatibleWithSDK(keyLength, to<StructType>(&valueType))) {
			push(-3 + 2, dictOpcode + "GET");
			switch (op) {
				case DictOperation::GetFromMapping:
					if (resultAsSliceForStruct) {
						pushContinuationWithDefaultDictValue();
						push(-2, "IFNOT");
					} else {
						// if ok
						{
							startContinuation();
							StructCompiler sc{this, to<StructType>(&valueType)};
							sc.convertSliceToTuple();
							endContinuation();
						}
						// if fail
						{
							startContinuation();
							StructCompiler sc{this, to<StructType>(&valueType)};
							sc.createDefaultStruct(false);
							endContinuation();
						}
						push(-2, "IFELSE");
					}
					break;
				case DictOperation::GetFromArray:
					push(-1, "THROWIFNOT " + toString(TvmConst::RuntimeException::ArrayIndexOutOfRange));
					if (!resultAsSliceForStruct) {
						StructCompiler sc{this, to<StructType>(&valueType)};
						sc.convertSliceToTuple();
					}
					break;
				case DictOperation::Fetch: {
					StructCompiler sc{&haveValue, to<StructType>(&valueType)};
					sc.convertSliceToTuple();
					fetchValue();
					break;
				}
				case DictOperation::Exist:
					checkExist();
					break;
			}
		} else {
			push(-3 + 2, dictOpcode + "GETREF");
			switch (op) {
				case DictOperation::GetFromMapping: {
					StackPusherHelper pusherHelper(&ctx());
					pusherHelper.push(-1 + 1, "CTOS");
					if (!resultAsSliceForStruct) {
						StructCompiler sc{&pusherHelper, to<StructType>(&valueType)};
						sc.convertSliceToTuple();
					}
					pushCont(pusherHelper.code());
					pushContinuationWithDefaultDictValue();
					push(-3, "IFELSE");
					break;
				}
				case DictOperation::GetFromArray:
					push(-1, "THROWIFNOT " + toString(TvmConst::RuntimeException::ArrayIndexOutOfRange));
					push(-1 + 1, "CTOS");
					if (!resultAsSliceForStruct) {
						StructCompiler sc{this, to<StructType>(&valueType)};
						sc.convertSliceToTuple();
					}
					break;
				case DictOperation::Fetch: {
					haveValue.push(0, "CTOS");
					StructCompiler sc{&haveValue, to<StructType>(&valueType)};
					sc.convertSliceToTuple();
					fetchValue();
					break;
				}
				case DictOperation::Exist:
					checkExist();
					break;
			}
		}
	} else if (isIn(valueCategory, Type::Category::Address, Type::Category::Contract) || isByteArrayOrString(&valueType)) {
		if (isByteArrayOrString(&valueType)) {
			push(-3 + 2, dictOpcode + "GETREF");
		} else {
			push(-3 + 2, dictOpcode + "GET");
		}

		switch (op) {
			case DictOperation::GetFromMapping:
				pushContinuationWithDefaultDictValue();
				push(-2, "IFNOT");
				break;
			case DictOperation::GetFromArray:
				push(-1, "THROWIFNOT " + toString(TvmConst::RuntimeException::ArrayIndexOutOfRange));
				break;
			case DictOperation::Fetch: {
				fetchValue();
				break;
			}
			case DictOperation::Exist:
				checkExist();
				break;
		}
	} else if (isIntegralType(&valueType) || isUsualArray(&valueType) || valueCategory == Type::Category::Mapping) {
		push(-3 + 2, dictOpcode + "GET");
		switch (op) {
			case DictOperation::GetFromMapping: {
				StackPusherHelper pusherHelper(&ctx());

				pusherHelper.preload(&valueType);
				pushCont(pusherHelper.code());

				pushContinuationWithDefaultDictValue();
				push(-3, "IFELSE");
				break;
			}
			case DictOperation::GetFromArray:
				push(-1, "THROWIFNOT " + toString(TvmConst::RuntimeException::ArrayIndexOutOfRange));
				preload(&valueType);
				break;
			case DictOperation::Fetch: {
				haveValue.preload(&valueType);
				fetchValue();
				break;
			}
			case DictOperation::Exist:
				checkExist();
				break;
		}
	} else if (valueCategory == Type::Category::VarInteger) {
		push(-3 + 2, dictOpcode + "GET");
		switch (op) {
			case DictOperation::GetFromMapping: {
				StackPusherHelper pusherHelper(&ctx());

				pusherHelper.preload(&valueType);
				pushCont(pusherHelper.code());

				pushContinuationWithDefaultDictValue();
				push(-3, "IFELSE");
				break;
			}
			case DictOperation::GetFromArray:
				solAssert(false, "TODO add test");
				push(-1, "THROWIFNOT " + toString(TvmConst::RuntimeException::ArrayIndexOutOfRange));
				preload(&valueType);
				break;
			case DictOperation::Fetch: {
				haveValue.preload(&valueType);
				fetchValue();
				break;
			}
			case DictOperation::Exist:
				checkExist();
				break;
		}
	} else {
		cast_error(node, "Unsupported value type: " + valueType.toString());
	}
}