/*
	Compile with: g++ crawl.cpp -o crawl -lcurl -std=c++11

*/
#include <cstring>			// memcpy () - aggregating data received by CURLOPT_WRITEFUNCTION (curl.h)
#include <unordered_map>	// for URLS - data structure to track all urls, their source and destinations
#include <unordered_set>	// for URLS
#include <curl/curl.h>		// http gathering
#include <fstream>			// write URL_directory, write_file ()
#include <queue>
#include <signal.h>			// block sigterm
#include <regex.h> 			// link recognition (vs. file)
#include <utility>
#include <json/json.h>
#include "Parser.h"
#include "robots.h"
#include "Neo4jConn.h"

#define REGEX_PATTERN "[a-zA-Z0-9./]*(.bmp|.gif|.jpg|.pdf|.png)"	// link recognition - regex.h
#define DATA_DIR "data/"		// fwrite () into this directory
#define INDENT_STEP 5			// print_column () - indent columns
#define COLUMN_WIDTH 80			// print_column () - width of block to print
#define HTML_BUF_SIZE 2000000	// for UrlData buffer (stores html data)
#define MSG_BUF_SIZE 4096		// link recognition (regex.h) error
int TIME_LIMIT = 60;
struct url_pair				
{
	std::string origin;
	std::string url;
};
std::unordered_map<int, std::unordered_set<int>*> URLS;
std::queue<url_pair*> URL_queue;	
int MAX_USED = 0;

/*
	Directory - this class will help us keep track of
		urls. Will use it on order to find url strings
		given id, and to find id given url strings 
		(reverse).
*/
class Directory
{
private:
	std::unordered_map<int, std::string> standard;
	std::unordered_map<std::string, int> reverse;
	std::ofstream ofile;
public:
	bool insert (int key, std::string value)
	{
		std::pair<std::string, int> rev_entry (value, key);
		if(reverse.insert(rev_entry).second)
		{
			standard[key] = value;
			return true;
		}
		else return false;
	}

	std::string get_value (int key)
	{
		return standard.at(key);
	}

	int get_key (std::string value)
	{
		return reverse.at(value);
	}

	int size ()
	{
		return standard.size();
	}

	// Used to write a text file listing
	//	id url
	//	id url
	//	...
	void write_file (std::string ofile_name)
	{
		ofile.open(ofile_name);
		if(!ofile.is_open())
		{
			std::cerr << "[!] " << ofile_name << " did not open.\n";
			return;
		}
		else
		{
			for (std::unordered_map<int, std::string>::iterator it = standard.begin(); it != standard.end(); it++)
			{
				ofile << it->first << " " << it->second << std::endl;
			}
		}
		ofile.close();
		std::cout << "[*] " << ofile_name << " written.\n";
	}
} URL_directory;

struct UrlData
{
	std::string source;
	FILE * file;
	char buffer [HTML_BUF_SIZE];
	int buf_size = 0;
	//int level;
};

void print_column (std::string str, int width)
{
	for (int i = 0; i < width && str[i] != '\0'; i++)
	{
		std::cout << str[i];
	}
	std::cout << std::endl;
}

std::string fix_rel_url (std::string source_url, std::string new_url)
{
	//std::cout << "source: " << source_url << " new: " << new_url << std::endl;
	if (new_url.front() == '/')
	{
		if (source_url.back() == '/')
		{
			new_url = source_url.substr(0,source_url.size() - 1) + new_url;
		}
		else
		{
			new_url = source_url + new_url;
		}
	}
	else if (new_url.substr(0,3) == "../")
	{
		if (source_url.back() == '/') source_url = source_url.substr(0, source_url.size() - 1);
		while (new_url.substr(0,3) == "../")
		{
			int pos = source_url.find_last_of("/");
			source_url = source_url.substr(0, pos);
			new_url = new_url.substr(3, new_url.size() - 3);
		}
		new_url = source_url + "/" + new_url;
	}
	else if (new_url.front() == '#')
	{
		if (source_url.back() == '/') new_url = source_url + new_url;
		else new_url = source_url + "/" + new_url;
	}
	//std::cout << "end source: " << source_url << " new: " << new_url << std::endl;
	return new_url;
}



bool is_link (char * str, int level)
{
	std::string url;
	regex_t regex;
	int reti;
	char buf [MSG_BUF_SIZE];

	// match image files, is_link() should return false for
	// thee matches, so we will negate the result
	reti = regcomp(&regex, REGEX_PATTERN, REG_EXTENDED);
	if (reti){fprintf(stderr, "Could not compile regex\n"); exit(1);}

	reti = regexec(&regex, str, 0, NULL, 0);
	if(!reti)
	{
		url = "[+] "; url += str;
		//print_indent(level * INDENT_STEP);print_column(url.c_str(), COLUMN_WIDTH);
		return true;
	}
	else if (reti == REG_NOMATCH)
	{	
		url = "[ ] "; url += str;
		//print_indent(level * INDENT_STEP);print_column(url.c_str(), COLUMN_WIDTH);
		return false;
	}
	else
	{
		regerror(reti, &regex, buf, sizeof(buf));
		fprintf(stderr, "Regex match failed: %s\n", buf);
		exit(1);
	}
}
std::string toString (char *c_str)
{
	std::string str = "";
	// Convert only the symbols, letters, and numbers
	// to strings. I had a hard time with an ASCII 13
	// (carriage regurn) being copied into the string.
	for (int i = 0; c_str[i] >= 33 && c_str[i] <= 126; i++)
	{
		str += c_str[i];	
	} 
	return str;
}

std::string toString (const char *c_str)
{
	std::string str = "";
	for (int i = 0; c_str[i] != '\0'; i++)
	{
		str += c_str[i];	
	} 
	return str;
}

// Just write header to disk
static size_t write_header (void *ptr, size_t size, size_t nmemb, void *data)
{
	int written = fwrite(ptr, size, nmemb, ((struct UrlData *)data)->file);
	return written;
}

// This will just write memory to a buffer, so that we can parse it
// when it is finished.
// called during curl_easy_perform ()
// ptr = data from internet
// size = #packets ?
// nmemb = amount data per packet
// data = my data
static size_t write_body (void *ptr, size_t size, size_t nmemb, void *data)
{
	UrlData *url_data = (struct UrlData *)data;
	char *mem_buf = url_data->buffer;
	int written = fwrite(ptr, size, nmemb, url_data->file);

	int mem_size = size * nmemb;
	memcpy(&(mem_buf[url_data->buf_size]), (char *)ptr, mem_size);
	url_data->buf_size += mem_size;
	mem_buf[url_data->buf_size] = 0;	// null terminate 1 index past end
	return written;
}

int mCurl (std::string origin_url, std::string source_url, int nth_curl)
{
	int source_url_id = URL_directory.get_key(source_url);

	UrlData header_data;		// will have url and buffer info
	UrlData body_data;		// and will be used to write to
					// output text files in data folder

	CURL *curl_handle;		// libcurl vars
	CURLcode res = CURLE_OK;	//

	std::string headerFilename = DATA_DIR + std::to_string(source_url_id) + "_head.txt";	// header output file
	std::string bodyFilename = DATA_DIR + std::to_string(source_url_id) + "_body.txt";	// body output file


	std::unordered_set<int> *target_URLS;				// target_URLS - this set will hold all urls that 
									// 	come from source_url
	if (URLS.count(source_url_id) == 0)				// URLS[source_url_id]
	{								//	- holds the set of target_URLS
		URLS[source_url_id] = new std::unordered_set<int> ();	//	- create the set if it doesn't exist
	}								//
	target_URLS = URLS[source_url_id];				//


	curl_global_init(CURL_GLOBAL_ALL);
	curl_handle = curl_easy_init();

	if(curl_handle)
	{

		/////////////////////////////////////////////
		//	SET UP CURL
		std::string heading = std::string("[" +  std::to_string(nth_curl) + "]-> " + source_url + "(" + std::to_string(source_url_id) + ")");
		print_column (heading, COLUMN_WIDTH);
		curl_easy_setopt(curl_handle, CURLOPT_TCP_KEEPALIVE, 1L);		//
		curl_easy_setopt(curl_handle, CURLOPT_URL, source_url.c_str());		// set curl url
		curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, write_header);	// set http header
		curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_body);	// write_body - write actual html code to disk
		curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, TIME_LIMIT);		// set connection timeout


		/////////////////////////////////////////////
		//	SET UP FILES
		header_data.file = fopen(headerFilename.c_str(), "wb");		// open access to output files
		if (header_data.file == NULL)					// they will be saved to the data folder
		{								//	- open header output files
			curl_easy_cleanup(curl_handle);				//
			return -1;						//
		}								//
		body_data.file = fopen(bodyFilename.c_str(), "wb");		//	- open body outpu file
		if (body_data.file == NULL)					//
		{								//
			curl_easy_cleanup(curl_handle);
			return -1;
		}

		/*struct UrlData			// for reference!
		{					// these will be passed to
			std::string source;		// for writing
			FILE * file;
			char buffer [HTML_BUF_SIZE];
			int buf_size = 0;
			//int level;
		};*/
		header_data.source = source_url;	// write_header()
		body_data.source = source_url;		// write_body()

		curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, &header_data);	// write_header(...,&header_data)
		curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, &body_data);		// write_body(...,&body_data)



 		/////////////////////////////////////////////
		//
		//	PERFORM THE CURL
		res = curl_easy_perform(curl_handle);
		//
		//
		/////////////////////////////////////////////


		
		// 1. Block SIGINT
		sigset_t old_mask, to_block;			// I included this because Linux
		sigemptyset(&to_block);				// kept killing this process.
		sigaddset(&to_block, SIGINT);			// I haven't tested too much to
		sigprocmask(SIG_BLOCK, &to_block, &old_mask);	// see if this really helps.

		fclose(header_data.file);
		fclose(body_data.file);

		// 3. Restore signal handling
		sigprocmask(SIG_SETMASK, &old_mask, NULL);


 		/////////////////////////////////////////////
		//
		//	CHECK CURL - did it finish ok?
		if (res != CURLE_OK)
		{
			fprintf(stdout, "[!]-> %s\n", curl_easy_strerror(res));

			remove(headerFilename.c_str());
			remove(bodyFilename.c_str());

			std::cerr << "[!] files removed." << std::endl;
		}
		curl_easy_cleanup(curl_handle);
		//
		//
 		/////////////////////////////////////////////


		// Parse the data
		std::list<std::string> *new_URLS = new std::list<std::string>();	// container to hold the new-found urls
		Parser Parser_(origin_url, source_url, (char *)body_data.buffer);	// create Parser
		Parser_.set_debug(false);						// change to "true" if you would like to see the parser in action
		Parser_.process();							// run the parse
		Parser_.print_info(std::string(DATA_DIR + std::to_string(source_url_id) + "_tags.txt"));	// print info to file
		Parser_.get_attribute_values("href", new_URLS);				// get list of all href attribute
											// values and store in new_URLS

		// Transmit the data
		std::list<Json::Value*> *json_creates = Parser_.getJson();
		Neo4jConn Connection ("out_row.json", "out_graph.json");
		for (std::list<Json::Value*>::iterator it = json_creates->begin(); it != json_creates->end(); it++)
		{
			Connection.NewTransaction();
			Connection.AddTransaction(*it);
			Connection.PostTransactionCommit();
			delete *it;				// delete the json entry that was allocated
		}						// for usage here
		delete json_creates;				// delete the container holding all
								// the "CREATE..." queries

		// Run all the newfound urls through robots.h
		// which will check for robots.txt entries and
		// disallow data colleciton
		std::string new_url;		// get the name of a new url that was found
		std::string robots_url;		// holds url that should have robots.txt (domain/robots.txt)
		for (std::list<std::string>::iterator it=new_URLS->begin(); it != new_URLS->end(); it++)
		{
			int new_id = URL_directory.size();
			new_url = fix_rel_url(source_url, *it);
			std::cout << "[+] " << new_url << std::endl;
			robots_url = robots::check(new_url);			// get the proposed robots url: domain/robots.txt
			if(robots_url != "")					// and perform curl on it (check())
			{							// it will return "" if no robots url
				URL_directory.insert(new_id, robots_url);	// if there is a robots url, insert the domain/robots.txt into the
				new_id++;					// directory so we dont check() it again.
			}
			if (!robots::is_blacklisted(new_url))			// if the new_url wasn't blacklisted in a robots.txt
			{
				if (URL_directory.insert(new_id, new_url))	// and if it is a new url (insert will return true)
				{
					target_URLS->insert(new_id);		// then add the new_id to the list of target_URLS
				}
			}
		}
		delete new_URLS;	// delete the container
		target_URLS = NULL;	// don't delete target_URLS, but,
					// we will set it to NULL. I'm not sure if
					// this is necessary.
					// remember, URLS[source_url_id] = target_URLS
					// so all the target urls are already in URLS

		std::string footer = "[+] " + std::to_string(URLS.at(source_url_id)->size()) + " urls found.";
		print_column (footer.c_str(), COLUMN_WIDTH);



		// Now we will add all the new urls to the
		// URL_queue
		for (std::unordered_set<int>::iterator it=URLS.at(source_url_id)->begin(); it != URLS.at(source_url_id)->end(); it++)
		{
			std::string url = URL_directory.get_value(*it);	// URLS is a map of source id -> target ids
									// get_value() gets the actual url name
			struct url_pair *url_pair_ = new url_pair ();	
			url_pair_->origin = source_url;
			url_pair_->url = url;
			URL_queue.push(url_pair_);			// we need the source url and target urlfor the
		}							// mCurl() call we will use on this new url
	}	
	else
	{
		fprintf(stderr, "[!] could not initialize curl.\n");
	}
	return res;
}

void clean_up ()
{
	for (std::unordered_map<int, std::unordered_set<int>*>::iterator it = URLS.begin(); it != URLS.end(); it++)
	{
		delete it->second;	// delete the set of target urls
	}				// remember URL[source url id] -> set of target url ids
}

void setFilenames (std::string file_out_opt, std::string row_arg, std::string &out_row_filename, std::string graph_arg, std::string &out_graph_filename)
{
	if(file_out_opt == "-r") 			// set row filename
	{	
		out_row_filename = row_arg;
		out_graph_filename = "out_graph.json";
	}
	else if (file_out_opt == "-g") 			// set graph filename
	{
		out_graph_filename = graph_arg;
		out_row_filename = "out_row.json";
	}
	else if (file_out_opt == "-rg") 		// set row and graph filenames
	{
		out_row_filename = row_arg;
		out_graph_filename = graph_arg;
	}
	else
	{
		out_row_filename = "out_row.json";
		out_graph_filename = "out_graph.json";
	}
	std::cout << "[ ] writing rows to " << out_row_filename << std::endl;
	std::cout << "[ ] writing graph to " << out_graph_filename << std::endl;
}	

int main (int argc, char* argv[])
{
	int n = 0;
	int max_curls;
	std::stringstream ss; ss << argv[2];
	ss >> max_curls;
	std::string url = std::string(argv[1]);

	if (url == "-q")								// Option -q, for query
	{
		if (argc < 4)
		{	
			std::cout << "webcrawler: query\n";
			std::cout << "crawl -q [\"query\"] [row|graph|row/graph] [-r|-g|-rg] [filename1] [filename2]\n"; 
			return 0;
		}
		Neo4jConn *Connection;
		std::string selection = argv[3];

		std::string file_out_opt = (argv[4] == NULL? "" : argv[4]);
		std::string out_row_arg = (argv[5] == NULL? "" : argv[5]);
		std::string out_row_filename;
		std::string out_graph_arg= (argv[6] == NULL? "" : argv[6]);
		std::string out_graph_filename;
		setFilenames (file_out_opt, out_row_arg, out_row_filename, out_graph_arg, out_graph_filename);

		if (selection == "row")							// get the output format
		{									//	- row
			Connection = new Neo4jConn (out_row_filename, "");		//	- graph	
		}									//	- row/graph
		else if (selection == "graph")						//
		{									//
			Connection = new Neo4jConn ("", out_graph_filename);		//
		} 									//
		else if (selection == "row/graph")					//
		{									//
			std::cout << "row/graph\n";					//
			Connection = new Neo4jConn (out_row_filename, out_graph_filename);	
		}									//
		else									//
		{									//
			std::cout << "Invalid 2nd argument\n";				//
			std::cout << "webcrawler: query\n";				//
			std::cout << "crawl -q [\"query\"] [row,graph,row/graph]\n"; 	//
			return 0;							//
		}

		Connection->NewTransaction();						// run the transaction
		Connection->AddTransaction(argv[2], selection);				//
		Connection->PostTransactionCommit();					//
		std::cout << "[OK] crawl completed the query.\n";
	}
	else if (url == "-pq")											// option -pq, for "pieced query"
	{
		if (argc < 6)
		{
			std::cout << "webcrawler: pieced query\n";
			std::cout << "crawl -pq [nodes/edges] [id/label/properties] [property] [value]\n"; 
			return 0;
		}
		std::string nodes_or_edges = argv[2];		// gather args
		std::string id_label_properties = argv[3];	//
		std::string property = argv[4];			//
		std::string value = argv[5];	
		//
		std::string file_out_opt = (argv[6] == NULL? "" : argv[6]);
		std::string out_row_arg = (argv[7] == NULL? "" : argv[7]);
		std::string out_row_filename;
		std::string out_graph_arg= (argv[8] == NULL? "" : argv[8]);
		std::string out_graph_filename;
		setFilenames (file_out_opt, out_row_arg, out_row_filename, out_graph_arg, out_graph_filename);

		Neo4jConn Connection (out_row_filename, out_graph_filename);
		Connection.NewTransaction();
		Connection.AddSearchTransaction(nodes_or_edges, id_label_properties, property, value);;
		Connection.PostTransactionCommit();
		std::cout << "[OK] crawl completed the query.\n";
	}
	else
	{								// the webcrawl
		robots::check(url);					// check starting url robots.txt
		if (robots::is_blacklisted(url)) return 0;		//

		URL_directory.insert(0, argv[1]);			// insert into the map of source url ids -> target url ids
		
		struct url_pair *url_pair_ = new url_pair;		// make an entry to load into the queue	
		url_pair_->origin = "";					//
		url_pair_->url = argv[1];				//
		URL_queue.push(url_pair_);				// load the queue

		while (URL_queue.size() > 0 && n <= max_curls)						
		{
			// Perform the curl, the curl will add urls
			// we find to the end of the URL_queue
			if(mCurl(URL_queue.front()->origin, URL_queue.front()->url, n) == CURLE_OK) n++;

			// delete memory used for the url_pair entry we ran curl with
			delete URL_queue.front();

			// remove the entry in the queue
			URL_queue.pop();
		}
									// URL_directory lists
									// id url
									// id url	
									// ...
		std::string ofile_name = DATA_DIR;			// prepare to write the URL_directory
		ofile_name += "directory.txt";				// to data/directory.txt
		URL_directory.write_file(ofile_name);			// write


		clean_up();

		std::cout << "[OK] crawl finished successfully\n";
	}
}
