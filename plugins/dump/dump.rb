module Groonga
  module Dump
    class DumpCommand < Command
      register("dump",
               [
               ])

      def run_body(input)
        writer.write("dump test")
      end
    end
  end
end
