/* ctype.h - character classification (kernel exports). */
#ifndef _CTYPE_H
#define _CTYPE_H
int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int islower(int c);
int isupper(int c);
int isspace(int c);
int isxdigit(int c);
int tolower(int c);
int toupper(int c);
#endif
