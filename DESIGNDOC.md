# **MyTerm \- Custom Terminal with X11GUI**

**Design Document**

## **\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_**

## **1\. Graphical User Interface with X11**

### **Implementation Technique**

The graphical interface was implemented using the X11 library (Xlib) to create a terminal emulator that operates independently of the standard console. Key components include:

* **Window Management**: XOpenDisplay(), XCreateSimpleWindow(), and XMapWindow() for creating and managing the application window.   
* **Event Handling**: XNextEvent() for capturing user input events including keyboard and mouse interactions  
* **Text Rendering**: XDrawString() for displaying text output in the window  
* **Buffer System**: Internal text buffer maintaining display content

## **2\. Execution of External Commands**

### **Implementation Technique**

* Process creation via `f`ork(`)` system call  
* Command execution using `execvp()` for external binaries  
* Process synchronization with `wait()` for foreground process management

### **Design Rationale**

* **Standard Unix Pattern**: The `fork()` \+ `execvp()` combination follows established Unix conventions for process creation and command execution  
* **Process Isolation**: Ensures separation between the shell process and executed commands, maintaining shell stability  
* **Shell Interpretation**: Utilizes "sh \-c" for proper interpretation of complex command lines and shell features

## **3\. Multiline Input Support**

### **Implementation Technique**

* Multi-line input detection through "\\n\\" continuation pattern recognition  
* Character handling using `read()` and `write()` system calls

### **Design Rationale**

* **Continuation Pattern**: Simple yet effective method for identifying multi-line commands without complex parsing overhead  
* **Stream-based I/O**: Leverages standard system calls for consistent character processing

## **4\. Input Redirection from Files**

### **Implementation Technique**

* File descriptor manipulation using `dup2()` system call  
* Symbol parsing for "\<" input redirection operator  
* File handling with `open()` system call in read-only mode  
* Command parsing to separate redirection specifications from executable commands

### **Design Rationale**

* **Explicit Redirection Control**: `dup2()` provides precise assignment of file descriptors, ensuring stdin (fd 0\) is properly redirected  
* **Unix File Descriptor Architecture**: Adheres to standard Unix design principles where stdin can be redirected from any file source  
* **Robust Parsing**: Simple token-based parsing effectively separates command from input file specification  
* **Error Handling**: Comprehensive file opening checks with appropriate error reporting

## **5\. Output Redirection to Files**

### **Implementation Technique**

* File descriptor redirection using `dup2()` system call  
* Symbol parsing for "\>" output redirection operator  
* File creation and truncation with `open()` using O\_WRONLY | O\_CREAT | O\_TRUNC flags  
* Permission setting with 0644 mode for created files

### **Design Rationale**

* **Precise Output Control**: `dup2()` ensures stdout (fd 1\) is correctly redirected to specified files  
* **File Management**: Proper file creation with truncation behavior matching standard shell conventions  
* **Permission Preservation**: Standard 0644 permissions maintain security while allowing user read/write access  
* **Atomic Operations**: Combined flag usage in `open()` ensures proper file state management

## **6\. Pipe Support Implementation**

### **Implementation Technique**

* Inter-process communication via `pipe()` system call  
* Multiple child process creation for piped command sequences  
* File descriptor redirection using `dup2()` for pipe connections

### 

### **Design Rationale**

* **Standard Pipeline Architecture**: Implements N-1 pipes for N commands, following conventional Unix pipeline design  
* **Process Chaining**: Each child process handles one command with proper input/output redirection to adjacent pipes  
* **Resource Management**: Ensures proper pipe cleanup and file descriptor closure

## **7\. multiWatch Command Implementation**

### **Implementation Technique**

* Parallel process execution via `fork()` for multiple commands  
* Output capture using temporary files (".temp.PID.txt")  
* I/O multiplexing with `poll()` system call  
* Signal handling for controlled termination

### **Design Rationale**

* **Temporary File Strategy**: Enables output capture from long-running processes without blocking issues inherent in pipe-based solutions  
* **Efficient I/O Handling**: `poll()` provides superior scalability compared to `select()` without file descriptor limitations  
* **Process Group Management**: `setpgid()` ensures proper signal delivery to command process groups  
* **Structured Output Format**: Maintains clear separation between different command outputs with timestamps

## **8\. Line Navigation Features (Ctrl+A and Ctrl+E)**

### **Implementation Technique**

* Control key combination detection (Ctrl+A \= ASCII 0x01, Ctrl+E \= ASCII 0x05)  
* Cursor position tracking within input buffer  
* Display updates using cursor positioning techniques

### **Design Rationale**

* **Direct Key Interpretation**: Efficient detection of control codes for immediate user feedback  
* **Cursor Index Management**: Maintains position state without requiring complete line redraws  
* **User Experience**: Mimics standard terminal behavior for familiar navigation

## **9\. Command Interruption via Signal Handling**

### **Implementation Technique**

* Signal handler implementation for SIGINT (Ctrl+C) and SIGTSTP (Ctrl+Z)  
* Process group signaling using `kill()` with negative PID values  
* Background job tracking and management

### **Design Rationale**

* **Process Group Signaling**: Essential for proper signal delivery to all children in command pipelines  
* **Handler Preservation**: Maintains and restores original signal handlers to preserve expected system behavior  
* **Job Control**: Implements background/foreground process management following Unix conventions

## **10\. Searchable Shell History System**

### **Implementation Technique**

* In-memory history storage with file-based persistence (".myterm\_history")  
* Approximate matching using longest common substring algorithm  
* Search mode activation via Ctrl+R keybinding

### **Design Rationale**

* **Persistent Storage**: File-based approach ensures history preservation across terminal sessions  
* **Fuzzy Matching Algorithm**: Longest common substring provides superior user experience over exact matching for partial command recall  
* **Modal Interface**: Clean separation between normal operation and search functionality

## **11\. File Name Auto-completion**

### **Implementation Technique**

* Directory enumeration using `opendir()` and `readdir()`  
* Prefix matching with longest common prefix calculation  
* Interactive selection interface for ambiguous completions

### **Design Rationale**

* **Efficient Directory Scanning**: Direct filesystem access provides current directory contents  
* **Common Prefix Resolution**: Standard approach for tab completion in modern shells  
* **User Interaction**: Interactive selection mode handles multiple matches effectively, following established shell conventions

**Extra features**

## **Scrolling Implementation**

### **Implementation Technique**

* **Vertical Scrolling**: Implemented using Up/Down arrow keys to navigate through command history and output  
* **Horizontal Scrolling**: Added Left/Right arrow key support for navigating wide output lines  
* **Viewport Management**: Maintains `scroll_x` and `scroll_y` variables to track visible region within the larger buffer  
* **Viewport Calculation**: Dynamically calculates visible content based on scroll position and window dimensions

### **Design Rationale**

* **Dual-Axis Navigation**: Supports both vertical and horizontal scrolling to handle extensive command output and long lines  
* **Incremental Scrolling**: Arrow keys provide fine-grained control over viewport positioning  
* **Viewport State Tracking**: Separate scroll position tracking for each tab maintains independent navigation contexts  
* **Real-time Redraw**: Immediate visual feedback during scrolling operations for responsive user experience

## **Support for the “exit” command**

### **Implementation Technique**

* **Command Detection**: String comparison for "exit" command in user input  
* **Resource Cleanup Protocol**: Comprehensive cleanup of all allocated resources  
* **Process Termination**: Systematic termination of all child processes and background jobs  
* **Graceful Shutdown**: Ordered shutdown sequence preserving system stability

### **Design Rationale**

#### **Comprehensive Resource Management**

* **Persistent Data Preservation**: The `save_history()` function ensures all command history is written to `.myterm_history` file before termination, maintaining user data across sessions  
* **Process Tree Cleanup**: Systematic termination of all child processes prevents zombie processes and ensures no orphaned processes remain running  
* **Background Job Handling**: Explicit termination of background jobs maintains system cleanliness and resource efficiency

## **Overall System Architecture**

### **Process Management Strategy**

* **Tab Isolation**: Separate shell processes per tab ensure operational independence and fault containment  
* **Process Group Coordination**: Essential for comprehensive signal handling and job control across command pipelines

### **Input/Output Subsystem**

* **Non-blocking Operations**: `fcntl()` with O\_NONBLOCK prevents blocking during command execution  
* **Multiplexed I/O Handling**: `select()`/`poll()` efficiently manage multiple I/O sources including X events, child process output, and user input

### **Memory Management Approach**

* **Bounded Resource Usage**: Fixed-size buffers prevent memory exhaustion while maintaining performance  
* **Comprehensive Cleanup**: Ensures proper release of all system resources including file descriptors, processes, and dynamic memory

## **Conclusion**

This design implements a feature-rich terminal. The architecture prioritizes reliability, performance, and compatibility with established shell conventions, delivering a robust platform for command-line operations with graphical interface capabilities. 