from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from dataclasses import dataclass, fields, is_dataclass
from enum import Enum, auto
from pathlib import Path
from typing import Sequence


class TokenKind(Enum):
    EOF = auto()
    NEWLINE = auto()
    IDENTIFIER = auto()
    NUMBER = auto()
    STRING = auto()

    REMEMBER = auto()
    SAY = auto()
    WHEN = auto()
    OTHERWISE = auto()
    REPEAT = auto()
    TRUE = auto()
    FALSE = auto()

    PLUS = auto()
    MINUS = auto()
    STAR = auto()
    SLASH = auto()
    PERCENT = auto()
    EQUAL = auto()
    EQUAL_EQUAL = auto()
    BANG_EQUAL = auto()
    GREATER = auto()
    GREATER_EQUAL = auto()
    LESS = auto()
    LESS_EQUAL = auto()
    LEFT_PAREN = auto()
    RIGHT_PAREN = auto()
    LEFT_BRACE = auto()
    RIGHT_BRACE = auto()

KEYWORDS = {
    "remember": TokenKind.REMEMBER,
    "say": TokenKind.SAY,
    "when": TokenKind.WHEN,
    "otherwise": TokenKind.OTHERWISE,
    "repeat": TokenKind.REPEAT,
    "true": TokenKind.TRUE,
    "false": TokenKind.FALSE,
}

@dataclass
class Token:
    kind: TokenKind
    value: object
    line: int
    column: int

    def __repr__(self):
        return f"Token({self.kind.name}, {self.value!r})"

class MerdenError(Exception):
    def __init__(self, message: str, line: int, column: int):
        super().__init__(message)
        self.message = message
        self.line = line
        self.column = column

class Lexer:
    def __init__(self, source: str):
        self.source = source
        self.index = 0
        self.line = 1
        self.column = 1

    def tokenize(self) -> list[Token]:
        tokens: list[Token] = []

        while not self._at_end():
            char = self._peek()

            if char in " \t\r":
                self._advance()
                continue

            if char == "\n":
                tokens.append(Token(TokenKind.NEWLINE, "\n", self.line, self.column))
                self._advance()
                continue

            if char == "#":
                self._skip_comment()
                continue

            if char == '"':
                tokens.append(self._read_string())
                continue

            if char.isdigit():
                tokens.append(self._read_number())
                continue

            if char.isalpha() or char == "_":
                tokens.append(self._read_identifier())
                continue

            line, column = self.line, self.column
            self._advance()

            single = {
                "+": TokenKind.PLUS,
                "-": TokenKind.MINUS,
                "*": TokenKind.STAR,
                "/": TokenKind.SLASH,
                "%": TokenKind.PERCENT,
                "(": TokenKind.LEFT_PAREN,
                ")": TokenKind.RIGHT_PAREN,
                "{": TokenKind.LEFT_BRACE,
                "}": TokenKind.RIGHT_BRACE,
            }
            if char in single:
                tokens.append(Token(single[char], char, line, column))
                continue

            if char == "=":
                if self._match("="):
                    tokens.append(Token(TokenKind.EQUAL_EQUAL, "==", line, column))
                else:
                    tokens.append(Token(TokenKind.EQUAL, "=", line, column))
                continue

            if char == "!" and self._match("="):
                tokens.append(Token(TokenKind.BANG_EQUAL, "!=", line, column))
                continue

            if char == ">":
                kind = TokenKind.GREATER_EQUAL if self._match("=") else TokenKind.GREATER
                tokens.append(Token(kind, ">=" if kind is TokenKind.GREATER_EQUAL else ">", line, column))
                continue

            if char == "<":
                kind = TokenKind.LESS_EQUAL if self._match("=") else TokenKind.LESS
                tokens.append(Token(kind, "<=" if kind is TokenKind.LESS_EQUAL else "<", line, column))
                continue

            raise MerdenError(f"Unknown character: {char!r}", line, column)

        tokens.append(Token(TokenKind.EOF, None, self.line, self.column))
        return tokens

    def _read_string(self) -> Token:
        line, column = self.line, self.column
        self._advance()  # opening quote
        chars: list[str] = []

        escapes = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\"}
        while not self._at_end() and self._peek() != '"':
            if self._peek() == "\n":
                raise MerdenError("String is not closed with a quotation mark", line, column)
            char = self._advance()
            if char == "\\":
                if self._at_end():
                    raise MerdenError("String is not closed with a quotation mark", line, column)
                escaped = self._advance()
                if escaped not in escapes:
                    raise MerdenError(f"Unknown escape sequence: \\{escaped}", self.line, self.column - 2)
                chars.append(escapes[escaped])
            else:
                chars.append(char)

        if self._at_end():
            raise MerdenError("String is not closed with a quotation mark", line, column)

        self._advance()  # closing quote
        return Token(TokenKind.STRING, "".join(chars), line, column)

    def _read_number(self) -> Token:
        line, column = self.line, self.column
        start = self.index
        while self._peek().isdigit():
            self._advance()
        return Token(TokenKind.NUMBER, int(self.source[start:self.index]), line, column)

    def _read_identifier(self) -> Token:
        line, column = self.line, self.column
        start = self.index
        while self._peek().isalnum() or self._peek() == "_":
            self._advance()
        value = self.source[start:self.index]
        return Token(KEYWORDS.get(value, TokenKind.IDENTIFIER), value, line, column)

    def _skip_comment(self) -> None:
        while not self._at_end() and self._peek() != "\n":
            self._advance()

    def _peek(self) -> str:
        return "\0" if self._at_end() else self.source[self.index]

    def _advance(self) -> str:
        char = self.source[self.index]
        self.index += 1
        if char == "\n":
            self.line += 1
            self.column = 1
        else:
            self.column += 1
        return char

    def _match(self, expected: str) -> bool:
        if self._at_end() or self.source[self.index] != expected:
            return False
        self._advance()
        return True

    def _at_end(self) -> bool:
        return self.index >= len(self.source)

@dataclass
class Program:
    statements: list[Statement]


@dataclass
class Statement:
    line: int
    column: int

@dataclass
class RememberStatement(Statement):
    name: str
    value: Expression

@dataclass
class SayStatement(Statement):
    value: Expression

@dataclass
class WhenStatement(Statement):
    condition: Expression
    then_branch: list[Statement]
    otherwise_branch: list[Statement] | None

@dataclass
class RepeatStatement(Statement):
    count: Expression
    body: list[Statement]

@dataclass
class Expression:
    line: int
    column: int

@dataclass
class NumberLiteral(Expression):
    value: int

@dataclass
class StringLiteral(Expression):
    value: str

@dataclass
class BoolLiteral(Expression):
    value: bool

@dataclass
class Variable(Expression):
    name: str

@dataclass
class UnaryExpression(Expression):
    operator: str
    right: Expression

@dataclass
class BinaryExpression(Expression):
    left: Expression
    operator: str
    right: Expression

class Parser:
    def __init__(self, tokens: Sequence[Token]):
        self.tokens = tokens
        self.current = 0

    def parse(self) -> Program:
        statements: list[Statement] = []
        self._skip_newlines()
        while not self._check(TokenKind.EOF):
            statements.append(self._statement())
            self._consume_statement_end()
            self._skip_newlines()
        return Program(statements)

    def _statement(self) -> Statement:
        if self._match(TokenKind.REMEMBER):
            keyword = self._previous()
            name = self._consume(TokenKind.IDENTIFIER, "A variable name is expected after 'remember'")
            self._consume(TokenKind.EQUAL, "The '=' sign is expected after the variable name")
            value = self._expression()
            return RememberStatement(keyword.line, keyword.column, str(name.value), value)

        if self._match(TokenKind.SAY):
            keyword = self._previous()
            return SayStatement(keyword.line, keyword.column, self._expression())

        if self._match(TokenKind.WHEN):
            keyword = self._previous()
            condition = self._expression()
            then_branch = self._block("A block in curly braces is expected after the 'when' condition")
            checkpoint = self.current
            self._skip_newlines()
            otherwise_branch = None
            if self._match(TokenKind.OTHERWISE):
                otherwise_branch = self._block("A block in curly braces is expected after 'otherwise'")
            else:
                # Newlines belong to the end of the when statement when there is no otherwise.
                self.current = checkpoint
            return WhenStatement(keyword.line, keyword.column, condition, then_branch, otherwise_branch)

        if self._match(TokenKind.REPEAT):
            keyword = self._previous()
            count = self._expression()
            body = self._block("A block in curly braces is expected after the repeat count")
            return RepeatStatement(keyword.line, keyword.column, count, body)

        token = self._peek()
        raise MerdenError("Expected a remember, say, when, or repeat command", token.line, token.column)

    def _block(self, message: str) -> list[Statement]:
        self._consume(TokenKind.LEFT_BRACE, message)
        body: list[Statement] = []
        self._skip_newlines()
        while not self._check(TokenKind.RIGHT_BRACE) and not self._check(TokenKind.EOF):
            body.append(self._statement())
            self._consume_statement_end()
            self._skip_newlines()
        self._consume(TokenKind.RIGHT_BRACE, "Code block is not closed with '}'")
        return body

    def _expression(self) -> Expression:
        return self._equality()

    def _equality(self) -> Expression:
        expression = self._comparison()
        while self._match(TokenKind.EQUAL_EQUAL, TokenKind.BANG_EQUAL):
            operator = str(self._previous().value)
            right = self._comparison()
            expression = BinaryExpression(expression.line, expression.column, expression, operator, right)
        return expression

    def _comparison(self) -> Expression:
        expression = self._term()
        kinds = (TokenKind.GREATER, TokenKind.GREATER_EQUAL, TokenKind.LESS, TokenKind.LESS_EQUAL)
        while self._match(*kinds):
            operator = str(self._previous().value)
            right = self._term()
            expression = BinaryExpression(expression.line, expression.column, expression, operator, right)
        return expression

    def _term(self) -> Expression:
        expression = self._factor()
        while self._match(TokenKind.PLUS, TokenKind.MINUS):
            operator = str(self._previous().value)
            right = self._factor()
            expression = BinaryExpression(expression.line, expression.column, expression, operator, right)
        return expression

    def _factor(self) -> Expression:
        expression = self._unary()
        while self._match(TokenKind.STAR, TokenKind.SLASH, TokenKind.PERCENT):
            operator = str(self._previous().value)
            right = self._unary()
            expression = BinaryExpression(expression.line, expression.column, expression, operator, right)
        return expression

    def _unary(self) -> Expression:
        if self._match(TokenKind.MINUS):
            token = self._previous()
            return UnaryExpression(token.line, token.column, str(token.value), self._unary())
        return self._primary()

    def _primary(self) -> Expression:
        if self._match(TokenKind.NUMBER):
            token = self._previous()
            return NumberLiteral(token.line, token.column, int(token.value))
        if self._match(TokenKind.STRING):
            token = self._previous()
            return StringLiteral(token.line, token.column, str(token.value))
        if self._match(TokenKind.TRUE, TokenKind.FALSE):
            token = self._previous()
            return BoolLiteral(token.line, token.column, token.kind is TokenKind.TRUE)
        if self._match(TokenKind.IDENTIFIER):
            token = self._previous()
            return Variable(token.line, token.column, str(token.value))
        if self._match(TokenKind.LEFT_PAREN):
            expression = self._expression()
            self._consume(TokenKind.RIGHT_PAREN, "Expected ')' after the expression")
            return expression
        token = self._peek()
        raise MerdenError("Expected a number, string, variable, or parenthesized expression", token.line, token.column)

    def _consume_statement_end(self) -> None:
        if self._check(TokenKind.NEWLINE, TokenKind.RIGHT_BRACE, TokenKind.EOF):
            return
        token = self._peek()
        raise MerdenError("Each command must end with a new line", token.line, token.column)

    def _skip_newlines(self) -> None:
        while self._match(TokenKind.NEWLINE):
            pass

    def _consume(self, kind: TokenKind, message: str) -> Token:
        if self._check(kind):
            return self._advance()
        token = self._peek()
        raise MerdenError(message, token.line, token.column)

    def _match(self, *kinds: TokenKind) -> bool:
        if self._check(*kinds):
            self._advance()
            return True
        return False

    def _check(self, *kinds: TokenKind) -> bool:
        return self._peek().kind in kinds

    def _advance(self) -> Token:
        if not self._check(TokenKind.EOF):
            self.current += 1
        return self._previous()

    def _peek(self) -> Token:
        return self.tokens[self.current]

    def _previous(self) -> Token:
        return self.tokens[self.current - 1]

class ValueType(Enum):
    NUMBER = auto()
    STRING = auto()
    BOOL = auto()


@dataclass(frozen=True)
class Symbol:
    c_name: str
    value_type: ValueType


class CGenerator:
    INTERPOLATION = re.compile(r"\{([^{}]+)\}")

    def __init__(self):
        self.lines: list[str] = []
        self.indent = 0
        self.scopes: list[dict[str, Symbol]] = [{}]
        self.variable_counter = 0
        self.loop_counter = 0

    def generate(self, program: Program) -> str:
        self.lines = [
            "#include <stdio.h>",
            "#include <string.h>",
            "#ifdef _WIN32",
            "#include <windows.h>",
            "#endif",
            "",
            "int main(void) {",
        ]
        self.indent = 1
        self._emit("#ifdef _WIN32")
        self._emit("SetConsoleOutputCP(CP_UTF8);")
        self._emit("#endif")
        for statement in program.statements:
            self._statement(statement)
        self._emit("return 0;")
        self.lines.append("}")
        return "\n".join(self.lines) + "\n"

    def _statement(self, statement: Statement) -> None:
        if isinstance(statement, RememberStatement):
            if statement.name in self.scopes[-1]:
                raise MerdenError(f"Variable '{statement.name}' already exists in this block", statement.line, statement.column)
            expression, value_type = self._expression(statement.value)
            c_name = f"md_value_{self.variable_counter}"
            self.variable_counter += 1
            self.scopes[-1][statement.name] = Symbol(c_name, value_type)
            c_type = {
                ValueType.NUMBER: "long long",
                ValueType.STRING: "const char *",
                ValueType.BOOL: "int",
            }[value_type]
            self._emit(f"{c_type} {c_name} = {expression};")
            return

        if isinstance(statement, SayStatement):
            self._say(statement)
            return

        if isinstance(statement, WhenStatement):
            condition, value_type = self._expression(statement.condition)
            if value_type not in (ValueType.BOOL, ValueType.NUMBER):
                raise MerdenError("The 'when' condition must be boolean or numeric", statement.line, statement.column)
            self._emit(f"if ({condition}) {{")
            self._enter_scope()
            for child in statement.then_branch:
                self._statement(child)
            self._leave_scope()
            self._emit("}")
            if statement.otherwise_branch is not None:
                self._emit("else {")
                self._enter_scope()
                for child in statement.otherwise_branch:
                    self._statement(child)
                self._leave_scope()
                self._emit("}")
            return

        if isinstance(statement, RepeatStatement):
            count, value_type = self._expression(statement.count)
            if value_type is not ValueType.NUMBER:
                raise MerdenError("The repeat count must be a number", statement.line, statement.column)
            loop_name = f"md_loop_{self.loop_counter}"
            self.loop_counter += 1
            self._emit(f"for (long long {loop_name} = 0; {loop_name} < ({count}); ++{loop_name}) {{")
            self._enter_scope()
            for child in statement.body:
                self._statement(child)
            self._leave_scope()
            self._emit("}")
            return

        raise AssertionError(f"Unsupported statement: {statement!r}")

    def _say(self, statement: SayStatement) -> None:
        if isinstance(statement.value, StringLiteral):
            value = statement.value.value
            matches = list(self.INTERPOLATION.finditer(value))
            if matches:
                format_parts: list[str] = []
                arguments: list[str] = []
                cursor = 0
                for match in matches:
                    format_parts.append(self._escape_printf(value[cursor:match.start()]))
                    name = match.group(1).strip()
                    symbol = self._find_symbol(name, statement.line, statement.column)
                    if symbol.value_type is ValueType.NUMBER:
                        format_parts.append("%lld")
                        arguments.append(symbol.c_name)
                    elif symbol.value_type is ValueType.STRING:
                        format_parts.append("%s")
                        arguments.append(symbol.c_name)
                    else:
                        format_parts.append("%s")
                        arguments.append(f"({symbol.c_name} ? \"true\" : \"false\")")
                    cursor = match.end()
                format_parts.append(self._escape_printf(value[cursor:]))
                format_string = "".join(format_parts) + "\\n"
                args = ", " + ", ".join(arguments) if arguments else ""
                self._emit(f'printf("{format_string}"{args});')
                return

        expression, value_type = self._expression(statement.value)
        if value_type is ValueType.NUMBER:
            self._emit(f'printf("%lld\\n", (long long)({expression}));')
        elif value_type is ValueType.STRING:
            self._emit(f'printf("%s\\n", {expression});')
        else:
            self._emit(f'printf("%s\\n", ({expression}) ? "true" : "false");')

    def _expression(self, expression: Expression) -> tuple[str, ValueType]:
        if isinstance(expression, NumberLiteral):
            return str(expression.value), ValueType.NUMBER
        if isinstance(expression, StringLiteral):
            return f'"{self._escape_c_string(expression.value)}"', ValueType.STRING
        if isinstance(expression, BoolLiteral):
            return ("1" if expression.value else "0"), ValueType.BOOL
        if isinstance(expression, Variable):
            symbol = self._find_symbol(expression.name, expression.line, expression.column)
            return symbol.c_name, symbol.value_type
        if isinstance(expression, UnaryExpression):
            right, value_type = self._expression(expression.right)
            if value_type is not ValueType.NUMBER:
                raise MerdenError("Unary minus can only be applied to a number", expression.line, expression.column)
            return f"(-({right}))", ValueType.NUMBER
        if isinstance(expression, BinaryExpression):
            left, left_type = self._expression(expression.left)
            right, right_type = self._expression(expression.right)
            operator = expression.operator

            if operator in {"+", "-", "*", "/", "%"}:
                if left_type is not ValueType.NUMBER or right_type is not ValueType.NUMBER:
                    raise MerdenError(f"Operator '{operator}' only works with numbers", expression.line, expression.column)
                return f"(({left}) {operator} ({right}))", ValueType.NUMBER

            if operator in {">", ">=", "<", "<="}:
                if left_type is not ValueType.NUMBER or right_type is not ValueType.NUMBER:
                    raise MerdenError(f"Operator '{operator}' only compares numbers", expression.line, expression.column)
                return f"(({left}) {operator} ({right}))", ValueType.BOOL

            if operator in {"==", "!="}:
                if left_type is not right_type:
                    raise MerdenError("Values of different types cannot be compared", expression.line, expression.column)
                if left_type is ValueType.STRING:
                    comparison = "== 0" if operator == "==" else "!= 0"
                    return f"(strcmp({left}, {right}) {comparison})", ValueType.BOOL
                return f"(({left}) {operator} ({right}))", ValueType.BOOL

        raise AssertionError(f"Unsupported expression: {expression!r}")

    def _find_symbol(self, name: str, line: int, column: int) -> Symbol:
        for scope in reversed(self.scopes):
            if name in scope:
                return scope[name]
        raise MerdenError(f"Variable '{name}' has not been created yet", line, column)

    def _enter_scope(self) -> None:
        self._emit("", raw=True)
        self.indent += 1
        self.scopes.append({})

    def _leave_scope(self) -> None:
        self.scopes.pop()
        self.indent -= 1

    def _emit(self, line: str, raw: bool = False) -> None:
        if raw and not line:
            return
        self.lines.append("    " * self.indent + line)

    @staticmethod
    def _escape_c_string(value: str) -> str:
        return (
            value.replace("\\", "\\\\")
            .replace('"', '\\"')
            .replace("\n", "\\n")
            .replace("\r", "\\r")
            .replace("\t", "\\t")
        )

    @classmethod
    def _escape_printf(cls, value: str) -> str:
        return cls._escape_c_string(value).replace("%", "%%")
    
def compile_source(source: str) -> tuple[list[Token], Program, str]:
    tokens = Lexer(source).tokenize()
    program = Parser(tokens).parse()
    c_code = CGenerator().generate(program)
    return tokens, program, c_code


def read_source(path: Path) -> str:
    if not path.exists():
        raise FileNotFoundError(f"File not found: {path}")
    if path.suffix != ".mrd":
        raise ValueError("The program file must have the .mrd extension")
    return path.read_text(encoding="utf-8")


def find_c_compiler() -> list[str]:
    configured = os.environ.get("CC")
    if configured:
        command = shlex.split(configured)
        if command and shutil.which(command[0]):
            return command
    for candidate in ("cc", "clang", "gcc"):
        if shutil.which(candidate):
            return [candidate]
    raise RuntimeError(
        "C compiler not found. On macOS, run 'xcode-select --install'; "
        "on Ubuntu/Debian, run 'sudo apt install build-essential'."
    )


def build_native(source_path: Path, c_code: str) -> tuple[Path, Path, list[str]]:
    compiler = find_c_compiler()
    build_dir = source_path.parent / ".mrd_build"
    build_dir.mkdir(exist_ok=True)
    c_path = build_dir / f"{source_path.stem}.c"
    c_path.write_text(c_code, encoding="utf-8")

    output_name = source_path.stem + (".exe" if os.name == "nt" else "")
    output_path = source_path.parent / output_name
    command = [*compiler, "-std=c11", "-O2", "-Wall", "-Wextra", str(c_path), "-o", str(output_path)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        details = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"C compiler exited with an error:\n{details}")
    return output_path, c_path, command


def format_error(error: MerdenError, source: str, filename: str) -> str:
    lines = source.splitlines()
    source_line = lines[error.line - 1] if 0 < error.line <= len(lines) else ""
    caret = " " * max(error.column - 1, 0) + "^"
    return (
        f"MerdenError: {error.message}\n"
        f"  {filename}:{error.line}:{error.column}\n"
        f"  {source_line}\n"
        f"  {caret}"
    )


def ast_to_json(program: Program) -> str:
    def serialize(value: object) -> object:
        if is_dataclass(value) and not isinstance(value, type):
            result = {"node": type(value).__name__}
            for field in fields(value):
                result[field.name] = serialize(getattr(value, field.name))
            return result
        if isinstance(value, list):
            return [serialize(item) for item in value]
        if isinstance(value, dict):
            return {key: serialize(item) for key, item in value.items()}
        return value

    return json.dumps(serialize(program), ensure_ascii=False, indent=2)


def create_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="compiler.py", description="Merden language compiler")
    subparsers = parser.add_subparsers(dest="command", required=True)

    for command, help_text in (
        ("build", "compile .mrd into a native program"),
        ("tokens", "show Lexer tokens"),
        ("ast", "show the Parser tree in JSON"),
        ("emit-c", "show the generated C code"),
    ):
        subparser = subparsers.add_parser(command, help=help_text)
        subparser.add_argument("file", type=Path, help="path to the .mrd file")

    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = create_argument_parser().parse_args(argv)
    source = ""
    try:
        source = read_source(args.file)
        tokens, program, c_code = compile_source(source)

        if args.command == "tokens":
            for token in tokens:
                print(f"{token.line:>3}:{token.column:<3} {token.kind.name:<15} {token.value!r}")
            return 0

        if args.command == "ast":
            print(ast_to_json(program))
            return 0

        if args.command == "emit-c":
            print(c_code, end="")
            return 0

        if args.command == "build":
            output_path, c_path, command = build_native(args.file.resolve(), c_code)
            print("✓ Lexer: tokens created")
            print("✓ Parser: AST built")
            print(f"✓ C code: {c_path}")
            print(f"✓ Compiler: {' '.join(command)}")
            print(f"✓ Program ready: {output_path}")
            if os.name == "nt":
                print(f"\nRun: .\\{output_path.name}")
            else:
                print(f"\nRun: ./{output_path.name}")
            return 0

        return 1
    except MerdenError as error:
        print(format_error(error, source, str(args.file)), file=sys.stderr)
    except (FileNotFoundError, ValueError, RuntimeError) as error:
        print(f"Error: {error}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())