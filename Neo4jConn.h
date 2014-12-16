#ifndef _dO_Ob_NEO4JCONN_H_
#define _dO_Ob_NEO4JCONN_H_

#include <cstring>
#include <iostream>	
#include <curl/curl.h>	
#include <json/json.h>
#include <stdlib.h>	// realloc, malloc

class Neo4jConn
{
private:
	struct MemoryStruct {
	  char *memory;
	  size_t size;
	};

	Json::Value JSON_DATA;
	int transaction_id = 0;


	std::string Trim(char * cstr)
	{
		std::string buffer = "";
		int int_value;
		for (int i = 0; cstr[i] != '\0'; i++)
	  	{
			int_value = (int)cstr[i];
			if (int_value >= 32)
			{
				buffer += cstr[i];
			}
		}
		return buffer;
	}

	static size_t
	WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp)
	{
		size_t realsize = size * nmemb;
		struct MemoryStruct *mem = (struct MemoryStruct *)userp;

		mem->memory = (char *)realloc(mem->memory, mem->size + realsize + 1);
		if(mem->memory == NULL) {
			/* out of memory! */ 
			printf("not enough memory (realloc returned NULL)\n");
			return 0;
		}

		memcpy(&(mem->memory[mem->size]), contents, realsize);
		mem->size += realsize;
		mem->memory[mem->size] = 0;

		return realsize;
	}
public:

	static void PrintStringChars (Json::Value data)
	{
		const char * cstr = data.toStyledString().c_str();
		for (int i = 0; cstr[i] != '\0'; i++)
	  	{
			std::cout << (int)cstr[i] << "|";
		}
		std::cout << std::endl;
	}
	
	void NewTransaction ()
	{
		JSON_DATA["statements"] = Json::Value(Json::arrayValue);
	}

	void AddTransaction (std::string str_stmt, std::string format)
	{
		Json::Value statement;
		statement["statement"] = str_stmt;
		statement["resultDataContents"] = Json::Value(Json::arrayValue);
		statement["resultDataContents"].append(format);
		JSON_DATA["statements"].append(statement);
	}

	void AddTransaction (Json::Value *json)
	{
		//std::cout << "Neo4jConn sees:" << json->toStyledString() << std::endl;
		JSON_DATA["statements"].append(*json);
	}

	static std::string Post (Json::Value data, std::string url)
	{
		CURL *curl_handle;
		CURLcode res;
		struct curl_slist *headers = NULL;
	 
		struct MemoryStruct chunk;

		chunk.memory = (char *)malloc(1);  	/* will be grown as needed by the realloc above */ 
		chunk.size = 0;    			/* no data at this point */ 

		headers = curl_slist_append(headers, "Accept: application/json; charset=UTF-8");
		headers = curl_slist_append(headers, "Content-Type: application/json");
	
		std::string data_str = data.toStyledString();
		std::cout << "Sending " << data_str << std::endl;	

		curl_handle = curl_easy_init();
		if(curl_handle) {
			curl_easy_setopt(curl_handle, CURLOPT_URL, url.c_str());			// url set
			curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);			// send POST message
			curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, data_str.c_str());//data);//stmts.c_str());
			curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDSIZE, data_str.size());	// Will i need CURLOPT_POSTFIELDSIZE_LARGE?
			curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);	// write response to memory
			curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);		// write response memory is &chunk
		

			curl_easy_setopt(curl_handle, CURLOPT_VERBOSE, 1L);

			res = curl_easy_perform(curl_handle);
			if(res != CURLE_OK)
			{
				std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
			}
			else
			{
				std::cout << "Received " << chunk.size << " bytes:\n";// << chunk.memory << "\n" << std::endl;
				Json::Value json_reply;
				Json::Reader reader;
				bool parse_success = reader.parse(chunk.memory, json_reply);
				std::cout << "Response: \n" << json_reply.toStyledString() << std::endl;
				ProcessResult(json_reply);
				
			}
			curl_easy_cleanup(curl_handle);	
		}
		if(chunk.memory) free(chunk.memory);
		curl_global_cleanup();
		return "";
	}

	static std::string PostTransactionCommit (Json::Value data)
	{
		return Post (data, "http://localhost:7474/db/data/transaction/commit");
	}

	std::string PostTransactionCommit ()
	{
		return Post (JSON_DATA, "http://localhost:7474/db/data/transaction/commit");
	}

	static void ProcessResult (Json::Value value)
	{
		std::cout << "Response (processed): \n";
		//PrintJsonTree(value, 0);


		// Print errors
		Json::Value errors = value["errors"];
		for (int i = 0; i < errors.size(); i++)
		{
			std::cout << "[!] " << errors[i] << std::endl;
		}

		Json::Value results = value["results"];


		
	
		for (int r = 0; r < results.size(); r++)
		{
			if (results[r].isMember("columns"))
			{
				// Print columns
				std::cout << "[\"columns\"] ";
				Json::Value columns = results[r]["columns"];
				for (int i = 0; i < columns.size(); i++)
				{
					std::cout << columns[i].asString() << " |";
				}
				std::cout << std::endl;
			}
			if (results[r].isMember("data"))
			{
				// Print data
				Json::Value data = results[r]["data"];
				for (int i = 0; i < data.size(); i++)
				{
					std::cout << "[\"data\"[" << i << "]]";
					if (data[i].isMember("row"))
					{
						Json::Value row = data[i]["row"];
						ProcessRow(row);
					}
					if (data[i].isMember("graph"))
					{
						Json::Value graph = data[i]["graph"];
						ProcessGraph(graph);
					}
					std::cout << std::endl;
				}
			}
		}
	}

	static void ProcessRow (Json::Value row)
	{
		for (int rw = 0; rw < row.size(); rw++)
		{
			std::cout << "[\"row\"] "; 
			for (Json::ValueIterator it = row[rw].begin(); it != row[rw].end(); it++)
			{
				std::cout << "\"" << it.key().asString() << "\":";
				if (it->type() == Json::arrayValue)
				{
					std::cout << "[";
					for (int e = 0; e < it->size(); e++)
					{
						//std::cout << (*it)[e].type() << " ";
						std::cout << (*it)[e].asString() << " ";
					}
					std::cout << "]";
				}	
				else
				{
					std::cout << (*it).asString();
				}			
				std::cout << " |";									
			}		
			//std::cout << std::endl;
		}
	}

	static void ProcessGraph(Json::Value graph)
	{
		Json::Value nodes = graph["nodes"];
		Json::Value relationships = graph["relationships"];
		std::cout << "[\"nodes\"] ";
		for (int i = 0; i < nodes.size(); i++)
		{
			std::cout << "(" << nodes[i]["id"].asString();
			for (int l = 0; l < nodes[i]["labels"].size(); l++)
			{
				std::cout << ":" << nodes[i]["labels"][l].asString();
			}
			std::cout << ") "; 
			for (Json::ValueIterator it = nodes[i]["properties"].begin(); it != nodes[i]["properties"].end(); it++)
			{
				std::cout << "\"" << it.key().asString() << "\":";
				if (it->type() == Json::arrayValue)
				{
					std::cout << "[";
					for (int e = 0; e < it->size(); e++)
					{
						//std::cout << (*it)[e].type() << " ";
						std::cout << (*it)[e].asString() << " ";
					}
					std::cout << "]";
				}	
				else
				{
					std::cout << (*it).asString();
				}			
				std::cout << " |";
			}
		}
		std::cout << "[\"relationships\"] ";
		for (int i = 0; i < relationships.size(); i++)
		{
			std::cout << "(" << relationships[i]["id"].asString();
			for (int l = 0; l < relationships[i]["labels"].size(); l++)
			{
				std::cout << ":" << relationships[i]["labels"][l].asString();
			}
			std::cout << ") "; 
			for (Json::ValueIterator it = relationships[i]["properties"].begin(); it != relationships[i]["properties"].end(); it++)
			{
				std::cout << "\"" << it.key().asString() << "\":";
				if (it->type() == Json::arrayValue)
				{
					std::cout << "[";
					for (int e = 0; e < it->size(); e++)
					{
						//std::cout << (*it)[e].type() << " ";
						std::cout << (*it)[e].asString() << " ";
					}
					std::cout << "]";
				}	
				else
				{
					std::cout << (*it).asString();
				}			
				std::cout << " |";
			}
		}
	}

	// http://stackoverflow.com/questions/4800605/iterating-through-objects-in-jsoncpp
	static void PrintJsonTree (Json::Value root, int depth)
	{ 
		depth++;
		if (root.size() > 0)
		{
			for (Json::ValueIterator it = root.begin(); it != root.end(); it++)
			{
				for (int d = 0; d < depth; d++)
				{
					std::cout << "   ";
				}
				PrintValue(it.key());
				std::cout << std::endl;
				PrintJsonTree(*it, depth);
			}
			return;
		}
		else
		{
			for (int d = 0; d < depth; d++)
			{
				std::cout << "   ";
			}
			PrintValue(root); 
			std::cout << std::endl;
		}
		return;
	}

	// http://stackoverflow.com/questions/4800605/iterating-through-objects-in-jsoncpp
	static void PrintValue (Json::Value val)
	{
	    if( val.isString() ) {
		std::cout << val.asString();
	    } else if( val.isBool() ) {
		std::cout << val.asBool(); 
	    } else if( val.isInt() ) {
		std::cout << val.asInt(); 
	    } else if( val.isUInt() ) {
		std::cout << val.asUInt(); 
	    } else if( val.isDouble() ) {
		std::cout << val.asDouble(); 
	    }
	    else 
	    {
		printf( "unknown type=[%d]", val.type() ); 
	    }
	}
};

#endif
