2/10/2026
Day 1:
Firstly setup the project on macOS. Then implemented the stack class functions , then wrote a test main to verify stack is working or not. Faced problems while pushing,popping but eventually figured it out and couldnt understand a way to compile because there was a linker failed fixed it by compiling it with older SDK of mac.
Made Timelineclass in which i initiliazed the constructor and then made record in which it was simple first make a new timelinenode and initilaize it with snapshot being passed then initliaze new nodes pre and next to nullptr if there was only one node it becomes head and tail and if not then tails next becomes new node and that new nodes prev becomes the tail and that new node becomes tha tail
3/10/2026
Day 2:
made writeheader function in which i had to write for stepcount and index offset . Made read sourceline function which basically checks whether a line is blank or not and stores it in a string and which can be used later on the string wont contain any blank spaces or anything it will contain useful lines. Made read forst word it will ignore the spaces in the starting then counter will increase and counter will start from when we find first word we read the first word in a string and return it.in read second word we do the same first ignore spaces before first word then ignore the first word and then ignore spaces after first word store it in a string and return it.
4/10/2026
Day 3:
started today by making validate program function which sees if the function is valid or not used readsourceline , firstword and second word in this function too now moving too write resolverecord
9/10/2026
Day 4:
Wrote writeresolverecord which writes the one line of code in file of [offset(8B)][string_size(4B)][string]. wrote readresolverecord which reads the file.