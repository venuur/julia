#include <iostream>
#include <map>
#include <ctime>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include "server.h"
#include "json.h"
#include "message_types.h"

using namespace std;
using namespace scgi;

/*

    TODO:
        - Make "incomplete expressions" work
        - Graphs!

*/

/////////////////////////////////////////////////////////////////////////////
// helpers that should really be part of the C++ standard library
/////////////////////////////////////////////////////////////////////////////

// convert a value to a string
template <class T> std::string to_string(const T& t)
{
    // convert a value to a string
    stringstream ss;
    ss<<t;
    return ss.str();
}

// parse a string from a value
template <class T> T from_string(const std::string& t)
{
    // parse a value from a string
    T ret;
    stringstream(t)>>ret;
    return ret;
}

// strip a string of leading and trailing whitespace
string strip(string str)
{
    // remove leading whitespace
    while (!str.empty())
    {
        if (str[0] == ' ' || str[0] == '\t' || str[0] == '\n' || str[0] == '\r')
            str = str.substr(1, str.size()-1);
        else
            break;
    }

    // remove trailing whitespace
    while (!str.empty())
    {
        if (str[str.size()-1] == ' ' || str[str.size()-1] == '\t' || str[str.size()-1] == '\n' || str[str.size()-1] == '\r')
            str = str.substr(0, str.size()-1);
        else
            break;
    }

    // return the result
    return str;
}

// convert a string to lowercase
string to_lower(string str)
{
    // convert a string to lowercase
    for (size_t j = 0; j < str.size(); j++)
        str[j] = tolower(str[j]);
    return str;
}

// convert a string to uppercase
string to_upper(string str)
{
    // convert a string to uppercase
    for (size_t j = 0; j < str.size(); j++)
        str[j] = toupper(str[j]);
    return str;
}

// split strings by a character
vector<string> split(string str, char separator)
{
    // split a string
    vector<string> result;
    string current_str;
    for (size_t i = 0; i < str.size(); i++)
    {
        if (str[i] == separator)
        {
            result.push_back(current_str);
            current_str = "";
        }
        else
            current_str += str.substr(i, 1);
    }
    result.push_back(current_str);
    return result;
}

// replace all occurrences of from with to in str
string str_replace(string str, string from, string to)
{
    // start from the beginning
    size_t pos = str.find(from, 0);
    while (pos != string::npos)
    {
        str.replace(pos, from.size(), to);
        pos = str.find(from, pos+to.size());
    }
    return str;
}

/////////////////////////////////////////////////////////////////////////////
// messages
/////////////////////////////////////////////////////////////////////////////

// a message
class message
{
public:
    // see julia-web.j for documentation
    uint8_t type;
    vector<string> args;
};

/////////////////////////////////////////////////////////////////////////////
// sessions
/////////////////////////////////////////////////////////////////////////////

// if a session hasn't been queried in this time, it dies
const int SESSION_TIMEOUT = 20; // in seconds

enum session_status
{
    SESSION_WAITING_FOR_PORT_NUM,
    SESSION_NORMAL,
    SESSION_TERMINATING,
};

// a session
struct session
{
    // time since watchdog was last "pet"
    time_t update_time;

    // to be sent to julia
    string inbox_std;

    // to be sent to julia
    vector<message> inbox;

    // to be sent to the client as MSG_OTHER_OUTPUT
    string outbox_std;

    // to be converted into messages
    string outbox_raw;

    // to be sent to the client
    vector<message> outbox;

    // process id of julia instance
    int pid;

    // write to julia_in[1], read from julia_out[0]
    int julia_in[2];
    int julia_out[2];

    // the socket for communicating to julia
    network::socket* sock;

    // io threads
    pthread_t inbox_proc;
    pthread_t outbox_proc;

    // when both threads have terminated, we can kill the session
    bool inbox_thread_alive;
    bool outbox_thread_alive;

    // the status of the session
    session_status status;
};

// a list of sessions
map<string, session> session_map;

// a mutex for accessing session_map
pthread_mutex_t session_mutex;

/////////////////////////////////////////////////////////////////////////////
// THREAD:  inbox_thread (from browser to julia)
/////////////////////////////////////////////////////////////////////////////

// add to the inbox regularly according to this interval
const int INBOX_INTERVAL = 10000; // in nanoseconds

// this thread sends input from the client to julia
void* inbox_thread(void* arg)
{
    // get the session token
    string session_token = (char*)arg;
    delete [] (char*)arg;

    // loop for the duration of the session
    while (true)
    {
        // lock the mutex
        pthread_mutex_lock(&session_mutex);

        // terminate if necessary
        if (session_map[session_token].status == SESSION_TERMINATING)
        {
            // unlock the mutex
            pthread_mutex_unlock(&session_mutex);

            // terminate
            break;
        }

        // get the inbox data
        string inbox = session_map[session_token].inbox_std;

        // if there is no inbox data and no messages to send, or if julia isn't ready, wait and try again
        if ((inbox == "" && session_map[session_token].inbox.empty()) || session_map[session_token].status != SESSION_NORMAL)
        {
            // unlock the mutex
            pthread_mutex_unlock(&session_mutex);

            // no data from client; pause before checking again
            timespec timeout;
            timeout.tv_sec = 0;
            timeout.tv_nsec = INBOX_INTERVAL;
            nanosleep(&timeout, 0);

            // try again
            continue;
        }

        // prepare for writing to julia
        int pipe = session_map[session_token].julia_in[1];

        // unlock the mutex
        pthread_mutex_unlock(&session_mutex);

        // write if there is data
        fd_set set;
        FD_ZERO(&set);
        FD_SET(pipe, &set);
        timeval select_timeout;
        select_timeout.tv_sec = 0;
        select_timeout.tv_usec = 100000;
        ssize_t bytes_written = 0;
        if (select(FD_SETSIZE, 0, &set, 0, &select_timeout))
            bytes_written = write(pipe, inbox.c_str(), inbox.size());

        // lock the mutex
        pthread_mutex_lock(&session_mutex);

        // write messages to julia
        for (size_t i = 0; i < session_map[session_token].inbox.size(); i++)
        {
            // get the message
            message msg = session_map[session_token].inbox[i];

            // write the message type
            string str_msg_type = " ";
            str_msg_type[0] = msg.type;
            session_map[session_token].sock->write(str_msg_type);

            // write the number of arguments
            string str_arg_num = " ";
            str_arg_num[0] = msg.args.size();
            session_map[session_token].sock->write(str_arg_num);

            // iterate through the arguments
            for (size_t j = 0; j < msg.args.size(); j++)
            {
                // write the size of the argument
                string str_arg_size = "    ";
                *((uint32_t*)(&(str_arg_size[0]))) = (uint32_t)msg.args[j].size();
                session_map[session_token].sock->write(str_arg_size);

                // write the argument
                session_map[session_token].sock->write(msg.args[j]);
            }
        }
        session_map[session_token].inbox.clear();

        // remove the written data from the inbox
        if (bytes_written < 0)
            session_map[session_token].inbox_std = "";
        else
            session_map[session_token].inbox_std = session_map[session_token].inbox_std.substr(bytes_written, session_map[session_token].inbox_std.size()-bytes_written);

        // unlock the mutex
        pthread_mutex_unlock(&session_mutex);
    }

    // lock the mutex
    pthread_mutex_lock(&session_mutex);

    // tell the watchdog that this thread is done
    session_map[session_token].inbox_thread_alive = false;

    // unlock the mutex
    pthread_mutex_unlock(&session_mutex);

    // terminate
    return 0;
}

/////////////////////////////////////////////////////////////////////////////
// THREAD:  outbox_thread (from julia to browser)
/////////////////////////////////////////////////////////////////////////////

// add to the outbox regularly according to this interval
const int OUTBOX_INTERVAL = 10000; // in nanoseconds

// check for the port number from julia according to this interval
const int PORT_NUM_INTERVAL = 10000; // in nanoseconds

// this thread waits for output from julia and stores it in a buffer (for later polling by the client)
void* outbox_thread(void* arg)
{
    // get the session token
    string session_token = (char*)arg;
    delete [] (char*)arg;

    // keep track of the output from julia
    string outbox_std;

    // loop for the duration of the session
    while (true)
    {
        // lock the mutex
        pthread_mutex_lock(&session_mutex);

        // terminate if necessary
        if (session_map[session_token].status == SESSION_TERMINATING)
        {
            // unlock the mutex
            pthread_mutex_unlock(&session_mutex);

            // terminate
            break;
        }

        // prepare for reading from julia
        char buffer[2];
        int pipe = session_map[session_token].julia_out[0];

        // unlock the mutex
        pthread_mutex_unlock(&session_mutex);

        // read if there is data
        fd_set set;
        FD_ZERO(&set);
        FD_SET(pipe, &set);
        timeval select_timeout;
        select_timeout.tv_sec = 0;
        select_timeout.tv_usec = 100000;
        ssize_t bytes_read = 0;
        if (select(FD_SETSIZE, &set, 0, 0, &select_timeout))
            bytes_read = read(pipe, buffer, 1);
        buffer[1] = 0;

        // lock the mutex
        pthread_mutex_lock(&session_mutex);

        // get the read data
        string new_data;
        if (bytes_read == 1)
            new_data = buffer;
        outbox_std += new_data;

        // send the outbox data to the client
        if (session_map[session_token].status == SESSION_NORMAL)
        {
            // try to read a character
            if (outbox_std != "")
            {
                // get the first character
                char c = outbox_std[0];

                // is it a control sequence?
                if (c == 0x1B)
                {
                    // if we don't have enough characters, try again later
                    if (outbox_std.size() < 2)
                    {
                        // unlock the mutex
                        pthread_mutex_unlock(&session_mutex);

                        // if we didn't get any new data from julia, wait before trying again
                        if (new_data == "")
                        {
                            timespec timeout;
                            timeout.tv_sec = 0;
                            timeout.tv_nsec = OUTBOX_INTERVAL;
                            nanosleep(&timeout, 0);
                        }

                        // try again
                        continue;
                    }

                    // check for multi-character escape sequences
                    if (outbox_std[1] == '[')
                    {
                        // find the last character in the sequence
                        int pos = -1;
                        for (int i = 2; i < outbox_std.size(); i++)
                        {
                            if (outbox_std[i] >= 64 && outbox_std[i] <= 126)
                            {
                                pos = i;
                                break;
                            }
                        }

                        // if we don't have enouch characters, try again later
                        if (pos == -1)
                        {
                            // unlock the mutex
                            pthread_mutex_unlock(&session_mutex);

                            // if we didn't get any new data from julia, wait before trying again
                            if (new_data == "")
                            {
                                timespec timeout;
                                timeout.tv_sec = 0;
                                timeout.tv_nsec = OUTBOX_INTERVAL;
                                nanosleep(&timeout, 0);
                            }

                            // try again
                            continue;
                        }
                        else
                        {
                            // eat the control sequence and output nothing
                            outbox_std = outbox_std.substr(pos+1, outbox_std.size()-(pos+1));
                            
                            // unlock the mutex
                            pthread_mutex_unlock(&session_mutex);

                            // keep going
                            continue;
                        }
                    }
                    else
                    {
                        // eat the two-character control sequence and output nothing
                        outbox_std = outbox_std.substr(2, outbox_std.size()-2);
                        
                        // unlock the mutex
                        pthread_mutex_unlock(&session_mutex);

                        // keep going
                        continue;
                    }
                }

                // just output the raw character
                session_map[session_token].outbox_std += c;
                outbox_std = outbox_std.substr(1, outbox_std.size()-1);
            }
        }

        // get the port number
        if (session_map[session_token].status == SESSION_WAITING_FOR_PORT_NUM)
        {
            // wait for a newline
            size_t newline_pos = outbox_std.find("\n");
            if (newline_pos == string::npos)
            {
                // unlock the mutex
                pthread_mutex_unlock(&session_mutex);

                // wait before trying again
                timespec timeout;
                timeout.tv_sec = 0;
                timeout.tv_nsec = PORT_NUM_INTERVAL;
                nanosleep(&timeout, 0);

                // try again
                continue;
            }

            // read the port number
            string num_string = outbox_std.substr(0, newline_pos);
            outbox_std = outbox_std.substr(newline_pos+1, outbox_std.size()-(newline_pos+1));
            int port_num = from_string<int>(num_string);

            // start
            session_map[session_token].sock = new network::socket;
            session_map[session_token].sock->connect("127.0.0.1", port_num);

            // switch to normal operation
            session_map[session_token].status = SESSION_NORMAL;

            // send a ready message
            message ready_message;
            ready_message.type = MSG_OUTPUT_READY;
            session_map[session_token].outbox.push_back(ready_message);
        }

        // try to read some data from the socket
        if (session_map[session_token].status == SESSION_NORMAL)
        {
            // get the socket before we unlock the mutex
            network::socket* sock = session_map[session_token].sock;

            // unlock the mutex
            pthread_mutex_unlock(&session_mutex);

            // try to read some data
            string data;
            if (sock->has_data())
                data += sock->read();
                            
            // unlock the mutex
            pthread_mutex_unlock(&session_mutex);

            // add the data to the outbox
            session_map[session_token].outbox_raw += data;
        }

        // try to convert the raw outbox data into messages
        string outbox_raw = session_map[session_token].outbox_raw;
        if (outbox_raw.size() >= 5)
        {
            // construct the message
            message msg;

            // get the message type
            msg.type = (*((uint8_t*)(&outbox_raw[0])))-1;

            // get the number of arguments
            uint8_t arg_num = *((uint8_t*)(&outbox_raw[1]));

            // try to read the arguments
            int pos = 2;
            for (uint8_t i = 0; i < arg_num; i++)
            {
                // make sure there is enough data left to read
                if (outbox_raw.size() < pos+4)
                    break;

                // get the size of this argument
                uint32_t arg_size = *((uint32_t*)(&outbox_raw[pos]));
                pos += 4;

                // make sure there is enough data left to read
                if (outbox_raw.size() < pos+arg_size)
                    break;

                // get the argument
                msg.args.push_back(outbox_raw.substr(pos, arg_size));
                pos += arg_size;
            }

            // check if we have a whole message
            if (msg.args.size() == arg_num)
            {
                // we have a whole message - eat it from outbox_raw
                session_map[session_token].outbox_raw = outbox_raw.substr(pos, outbox_raw.size()-pos);

                // add the message to the queue
                session_map[session_token].outbox.push_back(msg);
            }
        }

        // unlock the mutex
        pthread_mutex_unlock(&session_mutex);

        // nothing from julia; wait before trying again
        timespec timeout;
        timeout.tv_sec = 0;
        timeout.tv_nsec = OUTBOX_INTERVAL;
        nanosleep(&timeout, 0);
    }

    // lock the mutex
    pthread_mutex_lock(&session_mutex);

    // tell the watchdog that this thread is done
    session_map[session_token].outbox_thread_alive = false;

    // release the socket
    delete session_map[session_token].sock;

    // unlock the mutex
    pthread_mutex_unlock(&session_mutex);

    // terminate
    return 0;
}

/////////////////////////////////////////////////////////////////////////////
// THREAD:  watchdog_thread
/////////////////////////////////////////////////////////////////////////////

// the watchdog runs regularly according to this interval
const int WATCHDOG_INTERVAL = 100000000; // in nanoseconds

// this thread kills old sessions after they have timed out
void* watchdog_thread(void* arg)
{
    // run forever
    while (true)
    {
        // lock the mutex
        pthread_mutex_lock(&session_mutex);

        // get the current time
        time_t t = time(0);

        // start terminating old sessions
        for (map<string, session>::iterator iter = session_map.begin(); iter != session_map.end(); iter++)
        {
            if ((iter->second).status != SESSION_TERMINATING)
            {
                if (t-(iter->second).update_time >= SESSION_TIMEOUT)
                    (iter->second).status = SESSION_TERMINATING;
            }
        }

        // get a list of zombie sessions
        vector<string> zombie_list;
        for (map<string, session>::iterator iter = session_map.begin(); iter != session_map.end(); iter++)
        {
            if (!(iter->second).inbox_thread_alive && !(iter->second).outbox_thread_alive)
                zombie_list.push_back(iter->first);
        }

        // kill the zombies
        for (vector<string>::iterator iter = zombie_list.begin(); iter != zombie_list.end(); iter++)
        {
            // wait for the threads to terminate
            if (session_map[*iter].inbox_proc)
                pthread_join(session_map[*iter].inbox_proc, 0);
            if (session_map[*iter].outbox_proc)
                pthread_join(session_map[*iter].outbox_proc, 0);

            // close the pipes
            close(session_map[*iter].julia_in[1]);
            close(session_map[*iter].julia_out[0]);

            // kill the julia process
            kill(session_map[*iter].pid, 9);
            waitpid(session_map[*iter].pid, 0, 0);

            // remove the session from the map
            session_map.erase(*iter);

            // print the number of open sessions
            if (session_map.size() == 1)
                cout<<session_map.size()<<" open session.\n";
            else
                cout<<session_map.size()<<" open sessions.\n";
        }

        // unlock the mutex
        pthread_mutex_unlock(&session_mutex);

        // don't waste cpu time
        timespec timeout;
        timeout.tv_sec = 0;
        timeout.tv_nsec = WATCHDOG_INTERVAL;
        nanosleep(&timeout, 0);
    }

    // never reached
    return 0;
}

/////////////////////////////////////////////////////////////////////////////
// THREAD:  main_thread
/////////////////////////////////////////////////////////////////////////////

// the maximum number of concurrent sessions
const size_t MAX_CONCURRENT_SESSIONS = 4;

// generate a session token
string make_session_token()
{
    // add a random integer to a prefix
    return "SESSION_"+to_string(rand());
}

string respond(string session_token, string body)
{
    string header = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\n";
    header += "Set-Cookie: SESSION_TOKEN="+session_token+"\r\n";
    header += "\r\n";
    return header+body;
}

// create a session and return a session token
string create_session()
{
    // check if we've reached max capacity
    if (session_map.size() >= MAX_CONCURRENT_SESSIONS)
        return "";

    // create the session
    session session_data;

    // generate a session token
    string session_token = make_session_token();

    // keep the session alive for now
    session_data.inbox_thread_alive = true;
    session_data.outbox_thread_alive = true;
    session_data.status = SESSION_WAITING_FOR_PORT_NUM;

    // start the julia instance
    if (pipe(session_data.julia_in))
        return "";
    if (pipe(session_data.julia_out))
    {
        close(session_data.julia_in[0]);
        close(session_data.julia_in[1]);
        return "";
    }
    
    pid_t pid = fork();
    if (pid == -1)
    {
        close(session_data.julia_in[0]);
        close(session_data.julia_in[1]);
        close(session_data.julia_out[0]);
        close(session_data.julia_out[1]);
        return "";
    }
    if (pid == 0)
    {
        // this is the child process - redirect standard streams
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        dup2(session_data.julia_in[0], STDIN_FILENO);
        dup2(session_data.julia_out[1], STDOUT_FILENO);
        close(session_data.julia_in[0]);
        close(session_data.julia_in[1]);
        close(session_data.julia_out[0]);
        close(session_data.julia_out[1]);

        // set a high nice value
        if (nice(20) == -1)
            exit(1);

        // limit memory usage
        rlimit limits;
        limits.rlim_max = 200000000;
        limits.rlim_cur = limits.rlim_max;
        setrlimit(RLIMIT_AS, &limits);

        // acutally spawn julia instance
        execl("./julia", "julia", "./ui/webserver/julia-web.j", (char*)0);

        // if exec failed, terminate with an error
        exit(1);
    }
    close(session_data.julia_in[0]);
    close(session_data.julia_out[1]);

    // set the pid of the julia instance
    session_data.pid = pid;
    
    // start the inbox thread
    char* session_token_inbox = new char[256];
    strcpy(session_token_inbox, session_token.c_str());
    if (pthread_create(&session_data.inbox_proc, 0, inbox_thread, (void*)session_token_inbox))
    {
        delete [] session_token_inbox;
        session_data.inbox_proc = 0;
    }

    // start the outbox thread
    char* session_token_outbox = new char[256];
    strcpy(session_token_outbox, session_token.c_str());
    if (pthread_create(&session_data.outbox_proc, 0, outbox_thread, (void*)session_token_outbox))
    {
        delete [] session_token_outbox;
        session_data.outbox_proc = 0;
    }

    // set the start time
    session_data.update_time = time(0);

    // store the session
    session_map[session_token] = session_data;

    // print the number of open sessions
    if (session_map.size() == 1)
        cout<<session_map.size()<<" open session.\n";
    else
        cout<<session_map.size()<<" open sessions.\n";
    
    // return the session token
    return session_token;
}

// this function is called when an HTTP request is made - the response is the return value
string get_response(request* req)
{
    // lock the mutex
    pthread_mutex_lock(&session_mutex);

    // the session token
    string session_token;

    // the response
    message response_message;
    response_message.type = MSG_OUTPUT_NULL;

    // process input if there is any
    if (req->get_field_exists("request"))
    {
        // parse the request
        Json::Value request_root;
        Json::Reader reader;
        if (reader.parse(req->get_field_value("request"), request_root))
        {
            // extract the message from the request
            message request_message;
            request_message.type = request_root.get("message_type", -1).asInt();
            while (true)
            {
                string arg_name = "arg"+to_string(request_message.args.size());
                if (!request_root.isMember(arg_name))
                    break;
                request_message.args.push_back(request_root.get(arg_name, "").asString());
            }

            // check for the session cookie
            if (req->get_cookie_exists("SESSION_TOKEN"))
                session_token = req->get_cookie_value("SESSION_TOKEN");

            // check if the session is real
            if (session_token != "")
            {
                if (session_map.find(session_token) == session_map.end())
                    session_token = "";
            }

            // do some maintenance on the session if there is one
            if (session_token != "")
            {
                // pet the watchdog
                session_map[session_token].update_time = time(0);

                // make sure we don't get the port number
                if (session_map[session_token].status == SESSION_NORMAL)
                {
                    // catch any extra output from julia
                    if (session_map[session_token].outbox_std != "")
                    {
                        message output_message;
                        output_message.type = MSG_OUTPUT_OTHER;
                        output_message.args.push_back(session_map[session_token].outbox_std);
                        session_map[session_token].outbox_std = "";
                        session_map[session_token].outbox.push_back(output_message);
                    }

                    // merge messages of the same type when desirable
                    for (size_t i = 1; i < session_map[session_token].outbox.size(); i++)
                    {
                        // MSG_OUTPUT_OTHER
                        if (session_map[session_token].outbox[i].type == MSG_OUTPUT_OTHER)
                        {
                            if (session_map[session_token].outbox[i-1].type == MSG_OUTPUT_OTHER)
                            {
                                session_map[session_token].outbox[i-1].args[0] += session_map[session_token].outbox[i].args[0];
                                session_map[session_token].outbox.erase(session_map[session_token].outbox.begin()+i);
                                i--;
                            }
                        }
                    }
                }
            }

            // determine the type of request
            bool request_recognized = false;

            // MSG_INPUT_START
            if (request_message.type == MSG_INPUT_START)
            {
                // we recognize the request
                request_recognized = true;

                // create a new session
                session_token = create_session();
                if (session_token == "")
                {
                    // too many sessions
                    response_message.type = MSG_OUTPUT_FATAL_ERROR;
                    response_message.args.push_back("the server is currently at maximum capacity");
                }
            }

            // other messages go straight to julia
            if (!request_recognized)
            {
                // make sure we have a valid session
                if (session_token == "")
                {
                    response_message.type = MSG_OUTPUT_FATAL_ERROR;
                    response_message.args.push_back("session expired");
                }
                else
                {
                    // forward the message to julia
                    if (request_message.type != MSG_INPUT_POLL)
                        session_map[session_token].inbox.push_back(request_message);

                    // send the first message on the queue
                    if (!session_map[session_token].outbox.empty())
                    {
                        response_message = session_map[session_token].outbox[0];
                        session_map[session_token].outbox.erase(session_map[session_token].outbox.begin());
                        cout<<"message to browser: "<<int(response_message.type)<<"\n";
                    }
                }
            }
        }
    }

    // unlock the mutex
    pthread_mutex_unlock(&session_mutex);

    // convert the message to json
    Json::Value response_root;
    response_root["message_type"] = response_message.type;
    for (size_t i = 0; i < response_message.args.size(); i++)
        response_root["arg"+to_string(i)] = response_message.args[i];

    // return the header and response
    Json::StyledWriter writer;
    return respond(session_token, writer.write(response_root));
}

// program entrypoint
int main(int argc, char* argv[])
{
    // get the command line arguments
    int port_num = 1441;
    for (int i = 1; i < argc-1; i++)
    {
        if (string(argv[i]) == "-p")
            port_num = from_string<int>(argv[i+1]);
    }

    // seed the random number generator for generating session tokens
    srand(time(0));

    // ignore the SIGPIPE signal (when julia crashes or exits, we don't want to die too)
    struct sigaction act;
    act.sa_flags = 0;
    act.sa_handler = SIG_IGN;
    sigemptyset(&act.sa_mask);
    sigaction(SIGPIPE, &act, NULL);

    // initialize the mutex
    pthread_mutex_init(&session_mutex, 0);

    // start the watchdog thread
    pthread_t watchdog;
    pthread_create(&watchdog, 0, watchdog_thread, 0);

    // print a welcome message
    cout<<"server started on port "<<port_num<<".\n";

    // print the number of open sessions
    cout<<"0 open sessions.\n";

    // start the server
    run_server(port_num, &get_response);

    // never reached
    return 0;
}
