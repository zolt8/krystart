BUILDOPTS = 

all:
	./buildkrystart.sh $(BUILDOPTS)
clean:
	rm -rf built objects
	rm -f src/*.o src/*.gch
