// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)
#include <iostream>
#include <string>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node *next;
        Node(T value):data(value) , next(nullptr){};
    };
    Node *top;
    int32_t count;

public:
    // Implement these functions:
    Stack()
    { 
        top = nullptr;
        count = 0;
    }
    void push(const T &val)
    {
        if(count == MAX_STACK_DEPTH)
        {
            throw overflow_error("Stack is full");
        }
        Node* temp = new Node(val);
        temp->data = val;
        temp->next = top;
        top = temp;
        count++;
        // pushes the value on the stack if max limit is not reached yet.
    }
    T pop()
    {
        if(isEmpty())
        {
            throw underflow_error("Stack is empty");
        }
        Node* temp = top;
        T value = temp->data;
        top = top->next;
        delete temp;
        count--;
        return value;
        // pop the top value on the stack
    }
    T &peek()
    {
        if(isEmpty())
        {
            throw underflow_error("Stack is empty");
        }
        return top->data;
        // returns the top value on the stack
    }
    bool isEmpty()
    {
        return count == 0;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        int32_t count = 0;
        Node * temp = top;
        while(temp != nullptr && count < maxLen)
        {
            out[count] = temp->data;
            count++;
            temp = temp->next;
        }
        return count;
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
    }
};
// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot *data;
    TimelineNode *next;
    TimelineNode *prev;
};
class Timeline
{
    TimelineNode *head, *tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
        head = tail = nullptr;
        stepCount = 0;
    }
    void record(Snapshot *s)
    {
        TimelineNode* node = new TimelineNode;
        node->data = s;
        node->next = nullptr;
        node->prev = nullptr;
        if(head == nullptr)
        {
            head = tail = node;
        }
        else
        {
            tail->next = node;
            node->prev = tail;
            tail = node;
        }
        stepCount++;
        // add record in the timeline
    }
    TimelineNode *begin()
    {
        return head;
    }
    int32_t getStepCount()
    {
        return stepCount;
    }
};

// Core structs
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE *f, const TTDBHeader &h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);
    fwrite(&h.stepCount, sizeof(int32_t), 1 ,f);
    fwrite(&h.indexOffset, sizeof(int64_t),1,f);
    // placeholder for other two data members
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};

// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream &in, string &out)
{
    string line;
    while(getline(in,line))
    {
        if(!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        bool blank = true;
        for(int i = 0 ; i < line.size() ; i++)
        {
            if(line[i] != ' ' && line[i] != '\t')
            {
                blank = false;
                break;
            }
        }
        if(blank)
        {
            continue;
        }
        out = line;
        return true;
    }
    return false;
    // reads the next nonblank line
}
string firstWord(const string &line)
{
    int i = 0;
    while(i < line.size() && (line[i] == ' ' || line[i] == '\t'))
    {
        i++;
    }
    string word = "";
    while(i < line.size() && line[i] != ' ' && line[i] != '\t')
    {
        word = word + line[i];
        i++;
    }
    return word;
    // returns first word from the input string
}
string secondWord(const string &line)
{
    int i = 0;
    while(i < line.size() && (line[i] == ' ' || line[i] == '\t'))
    {
        i++;
    }
    while(i < line.size() && line[i] != ' ' && line[i] != '\t')
    {
        i++;
    }
    while(i < line.size() && (line[i] == ' ' || line[i] == '\t'))
    {
        i++;
    }
    string word = "";
    while(i < line.size() && line[i] != ' ' && line[i] != '\t')
    {
        word = word + line[i];
        i++;
    }
    return word;
    // returns the second word
}
bool validateProgram(const char *sourcePath)
{
    ifstream file(sourcePath);
    if(!file)
    {
        cout << "File not found" << endl;
        return false;
    }
    bool isinfunc = false;
    string line;
    while(readSourceLine(file,line))
    {
        string word = firstWord(line);
        if(word == "func")
        {
            if(isinfunc)
            {
                cout << "Nested function found" << line << endl;
                return false;
            }
            if(secondWord(line) == "")
            {
                cout << "Function has no name" << line << endl;
                return false;
            }
            isinfunc = true;
        }
        else if(word == "func_end")
        {
            if(!isinfunc)
            {
                cout << "Func end without starting func" << endl;
                return false;
            }
            isinfunc = false;
        }
    }
    if(isinfunc)
    {
        cout << "Missing func end" << endl;
        return false;
    }
    return true;
    // for each func defined there should be exactly one func_end and no nested funcs allowed - 
}
// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE *f, int64_t offsetField, const string &text)
{
    int64_t start = ftell(f);          
    if (start < 0)
    {
        return -1;
    } 
    int32_t size = (int32_t)text.size();
    fwrite(&offsetField, sizeof(int64_t), 1, f);   
    fwrite(&size, sizeof(int32_t), 1, f);          
    if (size > 0)
    {
        fwrite(text.data(), 1, size, f); 
    }           
    return start;
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
}
int64_t readResolveRecord(FILE *f, string &outText)
{
    int64_t offsetfield;
    int32_t size;
    if(fread(&offsetfield , sizeof(int64_t) , 1 , f) != 1)
    {
        return -1;
    }
    if (fread(&size, sizeof(int32_t), 1, f) != 1)
    {
        return -1;                                  
    }
    if (size < 0 || (uint64_t)size > MAX_SOURCE_BYTES)
    {
        return -1;                                  
    }
    outText.resize(size);
    if (size > 0 && fread(&outText[0], 1, size, f) != (size_t)size)
    {
        return -1;                                  
    }
    return offsetfield;
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
}
int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    ifstream in(sourcePath);
    if(!in)
    {
        cout << "Cannot open source file" << endl;
        return -1;
    }
    FILE *out = fopen(resolveBinPath, "wb+");
    if(!out)
    {
        cout << "Cannot create resolve.bin" << endl;
        return -1;
    }
    string line;
    while(readSourceLine(in,line))
    {
        string word = firstWord(line);
        int64_t position = writeResolveRecord(out,0,line);
        if(position < 0)
        {
            cout << "Writing error on resolve.bin" << endl;
            fclose(out);
            return -1;
        }
        if(word == "func")
        {
            string name = secondWord(line);
            for(int32_t i = 0 ; i < funcCount ; i++)
            {
                if(funcArray[i].funcName == name)
                {
                    cout << "Duplicate function : " << name << endl;
                    fclose(out);
                    return -1;
                }
            }
            if(funcCount >= MAX_FUNCS)
            {
                cout << "Too many functions ( max " << MAX_FUNCS << ")" << endl;
                fclose(out);
                return -1;
            }
            funcArray[funcCount].funcName = name;
            funcArray[funcCount].byteOffsetInResolveBin = position;
            funcCount++;
        }
        else if(word == "call")
        {
            string target = secondWord(line);
            if(target.empty())
            {
                cout << "Call with no target : " << line << endl;
                fclose(out);
                return -1;
            }
            if(patchCount >= MAX_PATCHES)
            {
                cout << "Too many call instructions (max " << MAX_PATCHES << ")" << endl;
                fclose(out);
                return -1;
            }
            patches[patchCount].byteOffsetOfOffsetField = position;
            patches[patchCount].targetFuncName = target;
            patchCount++;
        }
    }
    for (int32_t p = 0; p < patchCount; p++)
    {
        int64_t target = -1;
        for (int32_t i = 0; i < funcCount; i++)
        {
            if (funcArray[i].funcName == patches[p].targetFuncName)
            {
                target = funcArray[i].byteOffsetInResolveBin;
                break;
            }
        }
        if (target < 0)
        {
            cout << "Call to undefined function: " << patches[p].targetFuncName << endl;
            fclose(out);
            return -1;
        }
        if (fseek(out, (long)patches[p].byteOffsetOfOffsetField, SEEK_SET) != 0 || fwrite(&target, sizeof(int64_t), 1, out) != 1)
        {
            cout << "Patch error on resolve.bin" << endl;
            fclose(out);
            return -1;
        }
    }
    fclose(out);
    for (int32_t i = 0; i < funcCount; i++)
    {
        if (funcArray[i].funcName == "main")
            return funcArray[i].byteOffsetInResolveBin;
    }
    cout << "No main function" << endl;
    return -1;
    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error 
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens)
{
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
}
Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
    // build the snapshot based on the callStack given
}
void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }
    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");
    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);
    writeTdbg(timeline, "session.tdbg");

    return 0;
}