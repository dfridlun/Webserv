NAME = Webserv
CXX = c++
CXXFLAGS = -Wall -Wextra -Werror

SRCS = src/main.cpp \
		src/Listener.cpp \
		src/Eventloop.cpp \
		src/Connection.cpp \
		src/Server.cpp \
		src/HandleClient.cpp \
		src/Http.cpp \
		src/Response.cpp \
		src/Cgi.cpp


OBJS = $(SRCS:src/%.cpp=obj/%.o)

all: $(NAME)

GREEN = \033[0;32m
RESET = \033[0m

$(NAME): $(OBJS)
	$(CXX) $(OBJS) -o $(NAME)
	@echo "$(GREEN)Build succesful$(RESET)"

obj/%.o:src/%.cpp
	mkdir -p obj
	$(CXX) $(CXXFLAGS) -I include -c $< -o $@

clean:
	-rm -f $(OBJS)

fclean:	clean
	-rm -f $(NAME)

re: fclean all

.PHONY: all clean fclean re