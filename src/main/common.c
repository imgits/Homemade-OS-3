#include"common.h"
#include"assembly/assembly.h"

// static_assert(sizeof(int8_t) == 1);
static_assert(sizeof(uint8_t) == 1);
// static_assert(sizeof(int16_t) == 2);
static_assert(sizeof(uint16_t) == 2);
// static_assert(sizeof(int32_t) == 4);
static_assert(sizeof(uint32_t) == 4);
// static_assert(sizeof(int64_t) == 8);
static_assert(sizeof(uint64_t) == 8);
static_assert(sizeof(uintptr_t) == sizeof(void*));

void *memset(void *ptr, unsigned char value, size_t size){
	size_t a;
	for(a = 0; a < size; a++){
		((uint8_t*)ptr)[a] = value;
	}
	return ptr;
}

void *memcpy(void *dst, const void *src, size_t size){
	size_t a;
	for(a = 0; a < size; a++){
		((uint8_t*)dst)[a] = ((uint8_t*)src)[a];
	}
	return dst;
}

int strlen(const char *s){
	int len;
	for(len = 0; s[len] != '\0'; len++);
	return len;
}

static int strcmp2(const char *s1, const char *s2, size_t n, int ncmp){
	size_t i = 0;
	for(i = 0; ncmp == 0 || i < n ; i++){
		if(s1[i] != s2[i])
			return s1[i] > s2[i]? 1: -1;
		if(s1[i] == '\0')
			return 0;
	}
	return 0;
}

int strncmp(const char *s1, const char *s2, size_t n){
	return strcmp2(s1, s2, n, 1);
}

int strcmp(const char *s1, const char *s2){
	return strcmp2(s1, s2, 0, 0);
}

char *strncpy(char *dst, const char *src, size_t n){
	size_t a;
	for(a = 0; a < n; a++){
		dst[a] = src[a];
		if(dst[a] == '\0'){
			break;
		}
	}
	return dst;
}

int printString(const char *s, size_t length);

static int printHexadecimal(unsigned n){
	char s[12];
	int a;
	s[8] = '\0';
	for(a = 7; a >= 0; a--){
		unsigned b = (n & 15);
		s[a] = (b >= 10? b - 10 + 'a': b + '0');
		n = (n >> 4);
	}
	return printString(s, 8);
}

static int printUnsigned(unsigned n){
	char s[12];
	int a;
	a=11;
	s[a]='\0';
	do{
		a--;
		s[a] = (n % 10) + '0';
		n /= 10;
	}while(n != 0);
	return printString(s + a, 12 - a);
}

static int printSigned(int n){
	int printMinus = 0;
	if(n < 0){
		printString("-", 1);
		printMinus = 1;
		n = -n;
	}
	return printMinus + printUnsigned((unsigned)n);
}

static int printCharacter(int c){
	char t[4];
	t[0] = (char)c;
	t[1] = '\0';
	return printString(t, 1);
}

int printk(const char* format, ...){
	int printCount = 0;
	va_list argList;
	va_start(argList, format);
	int percentFlag = 0, formatLength = 0;
	int i;
	for(i = 0; format[i] != '\0'; i++){
		if(percentFlag == 0){
			if(format[i] == '%'){
				percentFlag = 1;
				formatLength = 0;
				continue;
			}
			char s[4];
			s[0] = format[i];
			s[1] = '\0';
			printCount += printString(s, 1);
			continue;
		}
		if(format[i] >= '0' && format[i] <= '9'){
			formatLength = formatLength * 10 + format[i] - '0';
			continue;
		}
		percentFlag = 0;
		switch(format[i]){
		case '%':
			printCount += printString("%", 1);
			break;
		case 'd':
			printCount += printSigned(va_arg(argList, int));
			break;
		case 'x':
			printCount += printHexadecimal(va_arg(argList, unsigned));
			break;
		case 'u':
			printCount += printUnsigned(va_arg(argList, unsigned));
			break;
		case 's':
			{
				const char *string  = va_arg(argList, const char*);
				printCount += printString(string, strlen(string));
			}
			break;
		case 'c':
			/*char is promoted to int when passed through ...*/
			printCount += printCharacter(va_arg(argList, int));
		}
		percentFlag = 0;
	}
	va_end(argList);
	return printCount;
}

void printAndHalt(const char *condition, const char *file, int line){
	printk("failure: %s %s %d", condition, file, line);
	// if the console does not display, use vm debugger to watch registers
	while(1){
		__asm__(
		"subl $0xf000be00, %1\n"
		"subl $0xf000be00, %2\n"
		:
		:"a"(line),"b"(file),"c"(condition)
		);
		hlt();
	}
}
