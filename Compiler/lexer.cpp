/*
=======
Lex.cpp
=======
Smalltalk lexical analyser
*/

///////////////////////

#include "stdafx.h"
#include "Lexer.h" 
#include "Str.h"

// We need these for strtoxl()
#include <limits.h>
//#include <math.h>
#include <malloc.h>
#include <stdlib.h>
#include <stdarg.h>

#ifdef DOWNLOADABLE
#include "downloadableresource.h"
#else
#include "..\compiler_i.h"
#endif

static const char COMMENTDELIM = '"';
static const char STRINGDELIM = '\'';
static const char CHARLITERAL = '$';
static const char LITERAL = '#';

///////////////////////

Lexer::Lexer()
{
	m_cp = NULL;
	m_token = NULL;
	m_lineno=0;
}

Lexer::~Lexer()
{
	delete[] m_token;
}

void Lexer::CompileError(int code, Oop extra)
{
	CompileErrorV(ThisTokenRange(), code, extra, 0);
}

void Lexer::CompileError(const TEXTRANGE& range, int code, Oop extra)
{
	CompileErrorV(range, code, extra, 0);
}

void Lexer::CompileErrorV(const TEXTRANGE& range, int code, ...)
{
	va_list extras;
	va_start(extras, code);
	_CompileErrorV(code, range, extras);
	va_end(extras);
}

void Lexer::SetText(const char* compiletext, int offset)
{
	m_tokenType=None;
	m_buffer = compiletext;
	m_cp = m_buffer.c_str();
	m_token = new char[m_buffer.size()+1];
	m_lineno=1;
	m_base=offset;
	AdvanceCharPtr(offset);
}

bool Lexer::IsAClosingChar(char ch) const
{
	// Return true if (ch) can close an expression
	switch (ch) 
	{
	case '.': 
	case ']': 
	case ')': 
	case ';':
	case '\"': 
	case '\'':
		return true;
	}
	return false;
}

// ANSI Binary chars are ...
// Note that this does include '-', and therefore whitespace is
// required to separate negation from a binary operator
bool Lexer::isAnsiBinaryChar(char ch)
{
	switch(ch)
	{
	case '!':
	case '%':
	case '&':
	case '*':
	case '+':
	case ',':
	case '/':
	case '<':
	case '=':
	case '>':
	case '?':
	case '@':
	case '\\':
	case '~':
	case '|':
	case '-':
		return true;
	}
	return false;
}

bool Lexer::IsASingleBinaryChar(char ch) const
{
	// Returns true if (ch) is a single binary characater
	// that can't be continued.
	//
	switch (ch) 
	{
	case '[': 
	case '(': 
	case ')': 
	case ']':
	case '#':
		return true;
	}
	return false;
}

inline void Lexer::SkipBlanks()
{
	// Skips blanks in the input stream but emits syntax colouring for newlines
	char ch=m_cc;
	while (ch && isspace(ch))
		ch=NextChar();
}

void Lexer::SkipComments()
{
	int commentStart = CharPosition();
	while (m_cc == COMMENTDELIM) 
	{
		char ch;
		do
		{
			ch = NextChar();
		}
		while (ch && ch != COMMENTDELIM);
		
		if (!ch)
		{
			// Break out at EOF 
			CompileError(TEXTRANGE(commentStart, CharPosition()), LErrCommentNotClosed);
		}
		
		const char* ep = m_cp;
		NextChar();
		SkipBlanks();
	}
}

inline bool issign(char ch)
{
	return ch == '-' || ch == '+';
}

void Lexer::ScanFloat()
{
	char ch = NextChar();
	if (isdigit(PeekAtChar()))
	{
		m_tokenType = FloatingConst;

		do
		{
			*tp++ = ch;
			ch = NextChar();
		}
		while (isdigit(ch));

		// Read the exponent, if any
		if (ch == 'e' || ch == 'd' || ch == 'q')
		{
			char peek = PeekAtChar();
			if (isdigit(peek) || issign(peek))
			{
				if (issign(peek))
				{
					NextChar();
					if (!isdigit(PeekAtChar()))
					{
						PushBack(peek);
						PushBack(ch);
						return;
					}
					
					ch = peek;
				}
				else
					ch = NextChar();

				*tp++ = 'e';

				do
				{
					*tp++ = ch;
					ch = NextChar();
				} while (isdigit(ch));
			}
		}
	}

	PushBack(ch);
}

int Lexer::DigitValue(char ch) const
{
	return ch >= '0' && ch <= '9'
			? ch - '0'
			: (ch >= 'A' && ch <= 'Z')
				? ch - 'A' + 10
				: -1;
}

// Note that the first digit has already been placed in the token buffer
void Lexer::ScanInteger(int radix)
{
	m_integer = 0;
	m_tokenType = SmallIntegerConst;
	int digit = DigitValue(PeekAtChar());

	int maxval = INT_MAX / radix;

	while (digit >= 0 && digit < radix)
	{
		*tp++ = NextChar();

		if (m_tokenType == SmallIntegerConst)
		{
			if (m_integer < maxval || (m_integer == maxval && digit <= INT_MAX % radix)) 
			{
				// we won't overflow, go ahead 
				m_integer = (m_integer * radix) + digit;
			}
			else
				// It will have to be left to Smalltalk to calc the large integer value
				m_tokenType = LargeIntegerConst;
		}

		digit = DigitValue(PeekAtChar());
	}
}

// Note that the first digit has already been placed in the token buffer
void Lexer::ScanExponentInteger()
{
	char e = NextChar();
	char* mark = tp;
	*tp++ = e;

	char ch = PeekAtChar();
	if (ch == '-' || ch == '+')
	{
		*tp++ = NextChar();
		ch = PeekAtChar();
	}

	if (isdigit(ch))
	{
		do
		{
			*tp++ = NextChar();
		}
		while (isdigit(PeekAtChar()));
		m_tokenType = LargeIntegerConst;
	}
	else
	{
		while (tp > mark)
		{
			tp--;
			PushBack(*tp);
		}
	}
}

void Lexer::ScanNumber()
{
	PushBack(*--tp);
	
	ScanInteger(10);
	
	char ch = PeekAtChar();
	
	// If they both read the same number of characters or the integer ended
	// in a radix specifier then we got ourselves an integer but we'll need
	// to check its in range later.
	//
	switch (ch)
	{
	case 'r':
		{
			if (m_tokenType != SmallIntegerConst)
				return;
			
			 if (m_integer >= 2 && m_integer <= 36)
			 {
				// Probably a short or long integer with a leading radix.
				
				*tp++ = NextChar();
				char* startSuffix = tp;
				int radix = m_integer;
				ScanInteger(radix);
				if (tp == startSuffix)
				{
					m_integer = radix;
					tp--;
					PushBack(ch);
				}
			 }
		}
		break;
		
	case 's':
		// A ScaledDecimal
		*tp++ = NextChar();
		
		// We must read over the trailing scale (optional)
		while(isdigit(PeekAtChar()))
			*tp++ = NextChar();
		
		m_tokenType = ScaledDecimalConst;
		break;
		
	case '.':
		// Its probably a floating value but might actually
		// be the end of statement
		ScanFloat();
		
		if (PeekAtChar() == 's')
		{
			// Its a scaled decimal - include any trailing digits
			*tp++ = NextChar();
			
			while(isdigit(PeekAtChar()))
				*tp++ = NextChar();
			
			m_tokenType = ScaledDecimalConst;
		}
		break;
		
	case 'e':
		{
			// Allow old St-80 exponent form, such as 1e6
			ScanExponentInteger();
		}

	default:
		break;
	};
}

// Read string up to terminating quote, ignoring embedded double quotes
void Lexer::ScanString(int stringStart)
{
	char ch;
	do
	{
		ch = NextChar();
		if (!ch)
		{
			CompileError(TEXTRANGE(stringStart, CharPosition()), LErrStringNotClosed);
		}
		else
		{
			if (ch==STRINGDELIM)
			{
				// Possible termination or double quotes
				if (PeekAtChar()==STRINGDELIM)
					ch = NextChar();
				else
					break;
			}
			*tp++ = ch;
		}
	}
	while (ch);
}

void Lexer::ScanName()
{
	while (isIdentifierSubsequent(NextChar()))
		*tp++ = m_cc;
	*tp = 0;
	const char* tok = ThisTokenText();
	if (!strcmp(tok, "true"))
		m_tokenType = TrueConst;
	else if (!strcmp(tok, "false"))
		m_tokenType = FalseConst;
	else if (!strcmp(tok, "nil"))
		m_tokenType = NilConst;
}

void Lexer::ScanQualifiedRef()
{
	const char* endLastWord = tp;
	*tp++ = m_cc;
	ScanName();
	if (m_tokenType != NameConst)
	{

		m_tokenType = NameConst;
	}
	else
	{
		if (m_cc == '.' && isLetter(PeekAtChar()))
			ScanQualifiedRef();
		else
			PushBack(m_cc);
	}
}

void Lexer::ScanIdentifierOrKeyword()
{
	m_tokenType = NameConst;
	ScanName();
	if (m_tokenType == NameConst && m_cc == '.' && isLetter(PeekAtChar()))
		ScanQualifiedRef();
	else
	{
		// It might be a Keyword terminated with a ':' (but not :=)
		// in which case we need to include it.
		if (m_cc == ':' && PeekAtChar() != '=') 
		{
			*tp++ = m_cc;
			m_tokenType = NameColon;
		}
		else 
		{
			PushBack(m_cc);
		}
	}
}

void Lexer::ScanSymbol()
{
	char* lastColon = NULL;
	while (isIdentifierFirst(m_cc))
	{
		*tp++ = m_cc;
		NextChar();
		while (isIdentifierSubsequent(m_cc))
		{
			*tp++ = m_cc;
			NextChar();
		}
		if (m_cc == ':')
		{
			lastColon = tp;
			*tp++ = m_cc;
			NextChar();
		}
	}

	if (lastColon != NULL && *(tp-1) != ':')
	{
		m_cp = m_cp - (tp - lastColon);
		tp = lastColon + 1;
		NextChar();
	}
	else
		PushBack(m_cc);
}

void Lexer::ScanBinary()
{
	while (isAnsiBinaryChar(m_cc))
	{
		*tp++ = m_cc;
		NextChar();
	}
	PushBack(m_cc);
}

void Lexer::ScanLiteral()
{
	// Literal; remove #
	tp--;	
	NextChar();

	if (isIdentifierFirst(m_cc))
	{
		ScanSymbol();
		m_tokenType = SymbolConst;
	}

	else if (isAnsiBinaryChar(m_cc))
	{
		ScanBinary();
		m_tokenType = SymbolConst;
	}

	else if (m_cc == STRINGDELIM)
	{
		// Quoted Symbol
		ScanString(CharPosition());
		m_tokenType = SymbolConst;
	}

	else if (m_cc == '(')
	{
		m_tokenType=ArrayBegin;
	}

	else if (m_cc == '[')
	{
		m_tokenType = ByteArrayBegin;
	}

	else if (m_cc == LITERAL)
	{
		// Second hash, so should be a constant expression ##(xxx)
		NextChar();
		if (m_cc != '(')
			CompileError(TEXTRANGE(CharPosition(), CharPosition()), LErrExpectExtendedLiteral);
		m_tokenType = ExprConstBegin;
	}

	else
	{
		m_thisTokenRange.m_stop = CharPosition();
		CompileError(LErrExpectConst);
	}
}

Lexer::TokenType Lexer::NextToken()
{
	m_lastTokenRange = m_thisTokenRange;
	NextChar();
	
	// Skip blanks and comments
	SkipBlanks();
	SkipComments();
	
	// Start remembering this token
	int start = CharPosition();
	m_thisTokenRange.m_start = start;
	
	tp = m_token;
	char ch = m_cc;
	
	*tp++ = ch;
	
	if (!ch)
	{
		// Hit EOF straightaway - so be careful not to write another Null term into the token
		// as this would involve writing off the end of the token buffer
		m_tokenType=Eof;
		m_thisTokenRange.m_stop = start;
		m_thisTokenRange.m_start = start + 1;
	}
	else
	{
		if (isIdentifierFirst(ch))
		{
			ScanIdentifierOrKeyword();
		}
		
		else if (isdigit(ch))
		{
			ScanNumber();
		}
		
		else if (ch == '-' && isdigit(PeekAtChar()))
		{
			Step();
			ScanNumber();
			if (m_tokenType == SmallIntegerConst)
				m_integer *= -1;
		}
		else if (ch == LITERAL) 
		{
			ScanLiteral();
		}

		else if (ch==STRINGDELIM) 
		{
			int stringStart = CharPosition();
			// String constant; remove quote
			tp--;
			ScanString(stringStart);
			
			m_tokenType = StringConst;
		}
		
		else if (ch==CHARLITERAL) 
		{		
			// Character constant
			m_integer=NextChar();
			if (!m_integer)
			{
				int pos = CharPosition();
				CompileError(TEXTRANGE(pos,pos), LErrExpectChar);
			}
			m_tokenType=CharConst;
		}
		
		else if (ch=='^')
		{
			m_tokenType = Return;
		}
		
		else if (ch==':')
		{
			if (PeekAtChar() == '=')
			{
				m_tokenType = Assignment;
				*tp++ = NextChar();
			}
			else
				m_tokenType = Special;
		}
		
		else if (ch == ')')
		{
			m_tokenType = CloseParen;
		}
		
		else if (ch == '.')
		{
			m_tokenType = CloseStatement;
		}

		else if (ch == ']')
		{
			m_tokenType = CloseSquare;
		}

		else if (ch == ';')
		{
			m_tokenType = Cascade;
		}

		else if (IsASingleBinaryChar(ch)) 
		{
			// Single binary expressions
			m_tokenType = Binary;
		}
		
		else if (isAnsiBinaryChar(ch))
		{
			m_tokenType = Binary;
		}
		else
		{
			int pos = CharPosition();
			CompileError(TEXTRANGE(pos, pos), LErrBadChar);
		}
		
		*tp = '\0';
	}

	m_thisTokenRange.m_stop = CharPosition();
	return m_tokenType;
}


///////////////////////////////////////////////////////////////////////////////
// This is almost a copy of the CRT strtol() function, but because
// Smalltalk number syntax differs from that of C, we must replace it with
// something a little more restrictive. Other than that I've tried to leave
// it as similar as possible to those actually in the CRT.
///////////////////////////////////////////////////////////////////////////////

inline long Lexer::strtol (
    const char *nptr,
    char **endptr,
    int ibase
    )
{
    return (long) strtoxl(nptr, (const char**)endptr, ibase, 0);
}


/***
*strtol, strtoul(nptr,endptr,ibase) - Convert ascii string to long un/signed
*       int.
*
*Purpose:
*       Convert an ascii string to a long 32-bit value.  The base
*       used for the caculations is supplied by the caller.  The base
*       must be in the range 0, 2-36.  If a base of 0 is supplied, the
*       ascii string must be examined to determine the base of the
*       number:
*               (a) First char = '0', second char = 'x' or 'X',
*                   use base 16.
*               (b) First char = '0', use base 8
*               (c) First char in range '1' - '9', use base 10.
*
*       If the 'endptr' value is non-NULL, then strtol/strtoul places
*       a pointer to the terminating character in this value.
*       See ANSI standard for details
*
*Entry:
*       nptr == NEAR/FAR pointer to the start of string.
*       endptr == NEAR/FAR pointer to the end of the string.
*       ibase == integer base to use for the calculations.
*
*       string format: [whitespace] [sign] [0] [x] [digits/letters]
*
*Exit:
*       Good return:
*               result
*
*       Overflow return:
*               strtol -- LONG_MAX or LONG_MIN
*               strtoul -- ULONG_MAX
*               strtol/strtoul -- errno == ERANGE
*
*       No digits or bad base return:
*               0
*               endptr = nptr*
*
*Exceptions:
*       None.
*******************************************************************************/

/* flag values */
#define FL_UNSIGNED   1       /* strtoul called */
#define FL_NEG        2       /* negative sign found */
#define FL_OVERFLOW   4       /* overflow occured */
#define FL_READDIGIT  8       /* we've read at least one correct digit */
	
	
unsigned long Lexer::strtoxl (
    const char *nptr,
    const char **endptr,
    int ibase,
    int flags
    )
{
    const char *p;
    char c;
    unsigned long number;
    unsigned digval;
    unsigned long maxval;
	
    p = nptr;                       /* p is our scanning pointer */
    number = 0;                     /* start with zero */
	
    c = *p++;                       /* read char */
    while ( isspace((int)(unsigned char)c) )
		c = *p++;               /* skip whitespace */
	
    if (c == '-') {
		flags |= FL_NEG;        /* remember minus sign */
		c = *p++;
    }
    else if (c == '+')
		c = *p++;               /* skip sign */
	
	//////////////////////////////////////////////////
	// BSM MODIFICATION - We don't want the base
	// detect capability (specified as base 0)
	//////////////////////////////////////////////////
    //if (ibase < 0 || ibase == 1 || ibase > 36) {
    if (ibase <= 1 || ibase > 36) {
		/* bad base! */
		if (endptr)
			/* store beginning of string in endptr */
			*endptr = nptr;
		return 0L;              /* return 0 */
    }
    //else if (ibase == 0) {
    //        /* determine base free-lance, based on first two chars of
    //           string */
    //        if (c != '0')
    //                ibase = 10;
    //        else if (*p == 'x' || *p == 'X')
    //                ibase = 16;
    //        else
    //                ibase = 8;
    //}
	
	//////////////////////////////////////////////////
	// BSM MODIFICATION - We don't accept 0xNNN format
	//////////////////////////////////////////////////
    //if (ibase == 16) {
    //        /* we might have 0x in front of number; remove if there */
    //        if (c == '0' && (*p == 'x' || *p == 'X')) {
    //                ++p;
    //                c = *p++;       /* advance past prefix */
    //        }
    //}
	
    /* if our number exceeds this, we will overflow on multiply */
    maxval = ULONG_MAX / ibase;
	
	
    for (;;) {      /* exit in middle of loop */
		/* convert c to value */
		if ( isdigit((int)(unsigned char)c) )
			digval = c - '0';
		///////////////////////////////////////////////
		// BSM MODIFICATION - HERE CRT USES isalpha(),
		// we use isupper() which is true for A-Z only.
		///////////////////////////////////////////////
		else if ( isupper((int)(unsigned char)c) )
			digval = /*toupper(*/c/*)*/ - 'A' + 10;
		else
			break;
		if (digval >= (unsigned)ibase)
			break;          /* exit loop if bad digit found */
		
		/* record the fact we have read one digit */
		flags |= FL_READDIGIT;
		
		/* we now need to compute number = number * base + digval,
		but we need to know if overflow occured.  This requires
		a tricky pre-check. */
		
		if (number < maxval || (number == maxval &&
            (unsigned long)digval <= ULONG_MAX % ibase)) {
			/* we won't overflow, go ahead and multiply */
			number = number * ibase + digval;
		}
		else {
			/* we would have overflowed -- set the overflow flag */
			flags |= FL_OVERFLOW;
		}
		
		c = *p++;               /* read next digit */
    }
	
    --p;                            /* point to place that stopped scan */
	
    if (!(flags & FL_READDIGIT)) {
	/* no number there; return 0 and point to beginning of
		string */
		if (endptr)
			/* store beginning of string in endptr later on */
			p = nptr;
		number = 0L;            /* return 0 */
    }
    else if ( (flags & FL_OVERFLOW) ||
		( !(flags & FL_UNSIGNED) &&
		( ( (flags & FL_NEG) && (number > -LONG_MIN) ) ||
		( !(flags & FL_NEG) && (number > LONG_MAX) ) ) ) )
    {
		/* overflow or signed overflow occurred */
		///////////////////////////////////////////////
		// BSM MODIFICATION - DON'T CARE ABOUT errno
		///////////////////////////////////////////////
		//errno = ERANGE;
		if ( flags & FL_UNSIGNED )
			number = ULONG_MAX;
		else if ( flags & FL_NEG )
			number = (unsigned long)(-LONG_MIN);
		else
			number = LONG_MAX;
    }
	
    if (endptr != NULL)
		/* store pointer to char that stopped the scan */
		*endptr = p;
	
    if (flags & FL_NEG)
		/* negate result if there was a neg sign */
		number = (unsigned long)(-(long)number);
	
    return number;                  /* done. */
}
