# **MyTerm \- Custom Terminal with X11GUI**

**README**

## **Prerequisites**

### **System Requirements**

* Linux operating system  
* X11 development libraries  
* C compiler(gcc)

### **Required Libraries**

* X11 library (`libx11-dev`)  
* Standard C library

## **Installation & Compilation**

### **Step 1: Installing Dependencies**

sudo apt-get install libx11-dev gcc

### **Step 2: Compile the Project**

gcc myTerm.c \-o myTerm \-lX11

### **Step 3: Run the Application**

./myTerm

## **Usage Guide**

### **Basic Navigation**

* **Typing Commands**: Click on the terminal window and type commands normally  
* **New Tab**: Press Ctrl+T  
* **Tab Switching**: Use Ctrl+Tab or click on tab headers  
* **Scrolling**: Use arrow keys for vertical and horizontal scrolling

### 

### 

### **Command Execution**

* **Basic Commands**: Type any standard shell command and press Enter  
* **Multi-line Commands**: End lines with `\n\` to continue on next line

**Advanced Features**

#### **Input Redirection**

./program \< input.txt

#### **Output Redirection**

ls \> output.txt

#### **Combined Redirection**

./program \< input.txt \> output.txt

#### **Pipe Support**

ls \*.txt | wc \-l  
cat file.txt | grep "pattern" | sort

#### **multiWatch Command**

multiWatch \["date", "ls \-l", "pwd"\]

* Executes commands in parallel  
* Displays output with timestamps  
* Press Ctrl+C to stop monitoring

#### **History Search**

* Press **Ctrl+R** to enter search mode  
* Type search term and press Enter  
* Shows exact matches, if present, or closest matches  
* Press Esc to exit the search mode. 

#### **Auto-completion**

* Type partial filename(atleast 1 character of the filename must be entered) and press **Tab**  
* For multiple matches, select from the numbered list by simply typing in the choice and pressing enter.   
* Completes file names in current directory. 

**Exiting from the terminal**

* Enter the “exit” command. 

#### **Signal Handling**

* **Ctrl+C**: Interrupt running command  
* **Ctrl+Z**: Move command to background  
* **Ctrl+A**: Move cursor to start of line  
* **Ctrl+E**: Move cursor to end of line

## **File Structure**

25CS60R01\_project/  
|-- myTerm.c          		\# Main source code  
|-- README.md         	\# Readme file  
|-- DESIGNDOC.md      	\# Detailed design documentation  
|-- .myterm\_history   	\# Command history (auto-generated)

## **Troubleshooting**

### **Common Issues**

1. **Compilation Errors**  
   * Verify X11 development libraries are installed  
   * Check for missing dependencies  
2. **Font Rendering Issues**  
   * Application uses default system fonts  
   * Some special characters may not render properly  
3. **Command Not Found**  
   * Please check for spelling and syntax errors while typing in commands

### **Performance Notes**

* The terminal maintains last 10,000 commands in history  
* Large output may require scrolling for full visibility  
* multiWatch creates temporary files for output capture

## **Project Specifications**

* X11-based GUI terminal  
* Multi-tab support  
* External command execution  
* Multiline input  
* Input redirection  
* Output redirection  
* Pipe support  
* multiWatch command  
* Line navigation (Ctrl+A, Ctrl+E)  
* Signal handling (Ctrl+C, Ctrl+Z)  
* Searchable history (Ctrl+R)  
* File name auto-completion
