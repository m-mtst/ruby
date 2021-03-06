# frozen_string_literal: true

RSpec.describe "bundle binstubs <gem>" do
  context "when the gem exists in the lockfile" do
    it "sets up the binstub" do
      install_gemfile <<-G
        source "file://#{gem_repo1}"
        gem "rack"
      G

      bundle "binstubs rack"

      expect(bundled_app("bin/rackup")).to exist
    end

    it "does not install other binstubs" do
      install_gemfile <<-G
        source "file://#{gem_repo1}"
        gem "rack"
        gem "rails"
      G

      bundle "binstubs rails"

      expect(bundled_app("bin/rackup")).not_to exist
      expect(bundled_app("bin/rails")).to exist
    end

    it "does install multiple binstubs" do
      install_gemfile <<-G
        source "file://#{gem_repo1}"
        gem "rack"
        gem "rails"
      G

      bundle "binstubs rails rack"

      expect(bundled_app("bin/rackup")).to exist
      expect(bundled_app("bin/rails")).to exist
    end

    it "displays an error when used without any gem" do
      install_gemfile <<-G
        source "file://#{gem_repo1}"
        gem "rack"
      G

      bundle "binstubs"
      expect(exitstatus).to eq(1) if exitstatus
      expect(out).to include("`bundle binstubs` needs at least one gem to run.")
    end

    context "when generating bundle binstub outside bundler" do
      it "should abort" do
        install_gemfile <<-G
          source "file://#{gem_repo1}"
          gem "rack"
        G

        bundle "binstubs rack"

        File.open("bin/bundle", "wb") do |file|
          file.print "OMG"
        end

        sys_exec "bin/rackup"

        expect(last_command.stderr).to include("was not generated by Bundler")
      end
    end

    context "the bundle binstub" do
      before do
        if system_bundler_version == :bundler
          system_gems :bundler
        elsif system_bundler_version
          build_repo4 do
            build_gem "bundler", system_bundler_version do |s|
              s.executables = "bundle"
              s.bindir = "exe"
              s.write "exe/bundle", "puts %(system bundler #{system_bundler_version}\\n\#{ARGV.inspect})"
            end
          end
          system_gems "bundler-#{system_bundler_version}", :gem_repo => gem_repo4
        end
        build_repo2 do
          build_gem "prints_loaded_gems", "1.0" do |s|
            s.executables = "print_loaded_gems"
            s.write "bin/print_loaded_gems", <<-R
              specs = Gem.loaded_specs.values.reject {|s| Bundler.rubygems.spec_default_gem?(s) }
              puts specs.map(&:full_name).sort.inspect
            R
          end
        end
        install_gemfile! <<-G
          source "file://#{gem_repo2}"
          gem "rack"
          gem "prints_loaded_gems"
        G
        bundle! "binstubs bundler rack prints_loaded_gems"
      end

      let(:system_bundler_version) { Bundler::VERSION }

      it "runs bundler" do
        sys_exec! "#{bundled_app("bin/bundle")} install"
        expect(out).to eq %(system bundler #{system_bundler_version}\n["install"])
      end

      context "when BUNDLER_VERSION is set" do
        it "runs the correct version of bundler" do
          sys_exec "BUNDLER_VERSION='999.999.999' #{bundled_app("bin/bundle")} install"
          expect(exitstatus).to eq(42) if exitstatus
          expect(last_command.stderr).to include("Activating bundler (999.999.999) failed:").
            and include("To install the version of bundler this project requires, run `gem install bundler -v '999.999.999'`")
        end
      end

      context "when a lockfile exists with a locked bundler version" do
        it "runs the correct version of bundler when the version is newer" do
          lockfile lockfile.gsub(system_bundler_version, "999.999.999")
          sys_exec "#{bundled_app("bin/bundle")} install"
          expect(exitstatus).to eq(42) if exitstatus
          expect(last_command.stderr).to include("Activating bundler (999.999.999) failed:").
            and include("To install the version of bundler this project requires, run `gem install bundler -v '999.999.999'`")
        end

        it "runs the correct version of bundler when the version is older" do
          simulate_bundler_version "55"
          lockfile lockfile.gsub(system_bundler_version, "44.0")
          sys_exec "#{bundled_app("bin/bundle")} install"
          expect(exitstatus).to eq(42) if exitstatus
          expect(last_command.stderr).to include("Activating bundler (44.0) failed:").
            and include("To install the version of bundler this project requires, run `gem install bundler -v '44.0'`")
        end

        it "runs the correct version of bundler when the version is a pre-release" do
          simulate_bundler_version "55"
          lockfile lockfile.gsub(system_bundler_version, "2.12.0.a")
          sys_exec "#{bundled_app("bin/bundle")} install"
          expect(exitstatus).to eq(42) if exitstatus
          expect(last_command.stderr).to include("Activating bundler (2.12.0.a) failed:").
            and include("To install the version of bundler this project requires, run `gem install bundler -v '2.12.0.a'`")
        end
      end

      context "when update --bundler is called" do
        before { lockfile.gsub(system_bundler_version, "1.1.1") }

        it "calls through to the latest bundler version" do
          sys_exec! "#{bundled_app("bin/bundle")} update --bundler"
          expect(last_command.stdout).to eq %(system bundler #{system_bundler_version}\n["update", "--bundler"])
        end

        it "calls through to the explicit bundler version" do
          sys_exec "#{bundled_app("bin/bundle")} update --bundler=999.999.999"
          expect(exitstatus).to eq(42) if exitstatus
          expect(last_command.stderr).to include("Activating bundler (999.999.999) failed:").
            and include("To install the version of bundler this project requires, run `gem install bundler -v '999.999.999'`")
        end
      end

      context "without a lockfile" do
        it "falls back to the latest installed bundler" do
          FileUtils.rm bundled_app("Gemfile.lock")
          sys_exec! bundled_app("bin/bundle").to_s
          expect(out).to eq "system bundler #{system_bundler_version}\n[]"
        end
      end

      context "using another binstub", :ruby_repo do
        let(:system_bundler_version) { :bundler }
        it "loads all gems" do
          sys_exec! bundled_app("bin/print_loaded_gems").to_s
          expect(out).to eq %(["bundler-#{Bundler::VERSION}", "prints_loaded_gems-1.0", "rack-1.2"])
        end

        context "when requesting a different bundler version" do
          before { lockfile lockfile.gsub(Bundler::VERSION, "999.999.999") }

          it "attempts to load that version" do
            sys_exec bundled_app("bin/rackup").to_s
            expect(exitstatus).to eq(42) if exitstatus
            expect(last_command.stderr).to include("Activating bundler (999.999.999) failed:").
              and include("To install the version of bundler this project requires, run `gem install bundler -v '999.999.999'`")
          end
        end
      end
    end

    it "installs binstubs from git gems" do
      FileUtils.mkdir_p(lib_path("foo/bin"))
      FileUtils.touch(lib_path("foo/bin/foo"))
      build_git "foo", "1.0", :path => lib_path("foo") do |s|
        s.executables = %w[foo]
      end
      install_gemfile <<-G
        gem "foo", :git => "#{lib_path("foo")}"
      G

      bundle "binstubs foo"

      expect(bundled_app("bin/foo")).to exist
    end

    it "installs binstubs from path gems" do
      FileUtils.mkdir_p(lib_path("foo/bin"))
      FileUtils.touch(lib_path("foo/bin/foo"))
      build_lib "foo", "1.0", :path => lib_path("foo") do |s|
        s.executables = %w[foo]
      end
      install_gemfile <<-G
        gem "foo", :path => "#{lib_path("foo")}"
      G

      bundle "binstubs foo"

      expect(bundled_app("bin/foo")).to exist
    end

    it "sets correct permissions for binstubs" do
      with_umask(0o002) do
        install_gemfile <<-G
          source "file://#{gem_repo1}"
          gem "rack"
        G

        bundle "binstubs rack"
        binary = bundled_app("bin/rackup")
        expect(File.stat(binary).mode.to_s(8)).to eq("100775")
      end
    end

    context "when using --shebang" do
      it "sets the specified shebang for the the binstub" do
        install_gemfile <<-G
          source "file://#{gem_repo1}"
          gem "rack"
        G

        bundle "binstubs rack --shebang jruby"

        expect(File.open("bin/rackup").gets).to eq("#!/usr/bin/env jruby\n")
      end
    end
  end

  context "when the gem doesn't exist" do
    it "displays an error with correct status" do
      install_gemfile <<-G
        source "file://#{gem_repo1}"
      G

      bundle "binstubs doesnt_exist"

      expect(exitstatus).to eq(7) if exitstatus
      expect(out).to include("Could not find gem 'doesnt_exist'.")
    end
  end

  context "--path" do
    it "sets the binstubs dir" do
      install_gemfile <<-G
        source "file://#{gem_repo1}"
        gem "rack"
      G

      bundle "binstubs rack --path exec"

      expect(bundled_app("exec/rackup")).to exist
    end

    it "setting is saved for bundle install", :bundler => "< 2" do
      install_gemfile <<-G
        source "file://#{gem_repo1}"
        gem "rack"
        gem "rails"
      G

      bundle! "binstubs rack", forgotten_command_line_options([:path, :bin] => "exec")
      bundle! :install

      expect(bundled_app("exec/rails")).to exist
    end
  end

  context "with --standalone option" do
    before do
      install_gemfile <<-G
        source "file://#{gem_repo1}"
        gem "rack"
      G
    end

    it "generates a standalone binstub" do
      bundle! "binstubs rack --standalone"
      expect(bundled_app("bin/rackup")).to exist
    end

    it "generates a binstub that does not depend on rubygems or bundler" do
      bundle! "binstubs rack --standalone"
      expect(File.read(bundled_app("bin/rackup"))).to_not include("Gem.bin_path")
    end

    context "when specified --path option" do
      it "generates a standalone binstub at the given path" do
        bundle! "binstubs rack --standalone --path foo"
        expect(bundled_app("foo/rackup")).to exist
      end
    end
  end

  context "when the bin already exists" do
    it "doesn't overwrite and warns" do
      FileUtils.mkdir_p(bundled_app("bin"))
      File.open(bundled_app("bin/rackup"), "wb") do |file|
        file.print "OMG"
      end

      install_gemfile <<-G
        source "file://#{gem_repo1}"
        gem "rack"
      G

      bundle "binstubs rack"

      expect(bundled_app("bin/rackup")).to exist
      expect(File.read(bundled_app("bin/rackup"))).to eq("OMG")
      expect(out).to include("Skipped rackup")
      expect(out).to include("overwrite skipped stubs, use --force")
    end

    context "when using --force" do
      it "overwrites the binstub" do
        FileUtils.mkdir_p(bundled_app("bin"))
        File.open(bundled_app("bin/rackup"), "wb") do |file|
          file.print "OMG"
        end

        install_gemfile <<-G
          source "file://#{gem_repo1}"
          gem "rack"
        G

        bundle "binstubs rack --force"

        expect(bundled_app("bin/rackup")).to exist
        expect(File.read(bundled_app("bin/rackup"))).not_to eq("OMG")
      end
    end
  end

  context "when the gem has no bins" do
    it "suggests child gems if they have bins" do
      install_gemfile <<-G
        source "file://#{gem_repo1}"
        gem "rack-obama"
      G

      bundle "binstubs rack-obama"
      expect(out).to include("rack-obama has no executables")
      expect(out).to include("rack has: rackup")
    end

    it "works if child gems don't have bins" do
      install_gemfile <<-G
        source "file://#{gem_repo1}"
        gem "actionpack"
      G

      bundle "binstubs actionpack"
      expect(out).to include("no executables for the gem actionpack")
    end

    it "works if the gem has development dependencies" do
      install_gemfile <<-G
        source "file://#{gem_repo1}"
        gem "with_development_dependency"
      G

      bundle "binstubs with_development_dependency"
      expect(out).to include("no executables for the gem with_development_dependency")
    end
  end

  context "when BUNDLE_INSTALL is specified" do
    it "performs an automatic bundle install" do
      gemfile <<-G
        source "file://#{gem_repo1}"
        gem "rack"
      G

      bundle "config auto_install 1"
      bundle "binstubs rack"
      expect(out).to include("Installing rack 1.0.0")
      expect(the_bundle).to include_gems "rack 1.0.0"
    end

    it "does nothing when already up to date" do
      install_gemfile <<-G
        source "file://#{gem_repo1}"
        gem "rack"
      G

      bundle "config auto_install 1"
      bundle "binstubs rack", :env => { "BUNDLE_INSTALL" => 1 }
      expect(out).not_to include("Installing rack 1.0.0")
    end
  end
end
